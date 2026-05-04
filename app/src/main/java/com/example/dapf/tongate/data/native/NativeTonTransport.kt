package com.example.dapf.tongate.data.native

import android.util.Log
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.charset.StandardCharsets
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.Executors
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicLong

/**
 * JNI-обертка над TON ADNL transport.
 * Хранит нативный контекст и предоставляет API для отправки/получения пакетов.
 */
class NativeTonTransport private constructor() {
    enum class FrameType(val id: Int) {
        UNKNOWN(0),
        OPEN(1),
        DATA(2),
        CLOSE(3),
        ERROR(4);

        companion object {
            fun fromId(id: Int): FrameType = entries.firstOrNull { it.id == id } ?: UNKNOWN
        }
    }

    data class TransportConfig(
        val egressAdnlAddress: String,
        val clientPrivateKey: String,
        val authToken: String,
        /** IPv4 published in ADNL addressList (WLAN). Must not be 0.0.0.0 / 127.0.0.1. */
        val localUdpAdvertiseHost: String,
    )

    data class InboundFrame(
        val type: FrameType,
        val streamId: Int,
        val payload: ByteArray = ByteArray(0),
        val errorMessage: String? = null,
        val rawFrame: ByteArray = ByteArray(0),
    )

    /**
     * Result of waiting on a stream queue: frame, poll timeout, or [InterruptedException] from [java.util.concurrent.BlockingQueue.poll].
     */
    data class StreamAwaitResult(
        val frame: InboundFrame?,
        val interrupted: Boolean = false,
    ) {
        val timedOut: Boolean get() = frame == null && !interrupted
    }

    @Volatile
    private var nativeContextPtr: Long = 0L
    private val streamQueues = ConcurrentHashMap<Int, LinkedBlockingQueue<InboundFrame>>()
    private val pendingOpen = ConcurrentHashMap<Int, Long>() // streamId -> startedAtMs
    private val lateOpenAckLogged = ConcurrentHashMap<Int, Boolean>() // streamId -> logged?
    private val receiverRunning = AtomicBoolean(false)
    private val receiverExecutor = Executors.newSingleThreadExecutor()
    @Volatile
    private var runtimeConfig: TransportConfig? = null
    private val txFrames = AtomicLong(0)
    private val txBytes = AtomicLong(0)
    private val txPayloadBytes = AtomicLong(0)
    private val rxBytes = AtomicLong(0)
    private val openOk = AtomicLong(0)
    private val openFail = AtomicLong(0)
    private val openTimeout = AtomicLong(0)
    private val lateOpenAck = AtomicLong(0)
    private val droppedEmptyFrames = AtomicLong(0)
    private val jniCalls = AtomicLong(0)
    private val rttSumMs = AtomicLong(0)
    private val rttSamples = AtomicLong(0)
    @Volatile
    private var metricsWindowStartMs: Long = System.currentTimeMillis()

    /**
     * Инициализация ADNL-контекста.
     * Возвращает указатель/хэндл нативного контекста.
     */
    fun initialize(config: String, transportConfig: TransportConfig): Long {
        if (nativeContextPtr != 0L) return nativeContextPtr
        synchronized(this) {
            if (nativeContextPtr == 0L) {
                val configureCode = configureAdnlPeer(
                    transportConfig.egressAdnlAddress,
                    transportConfig.clientPrivateKey,
                    transportConfig.authToken,
                    transportConfig.localUdpAdvertiseHost,
                )
                if (configureCode != 0) {
                    Log.e(TAG, "configureAdnlPeer failed: code=$configureCode")
                    return 0L
                }
                nativeContextPtr = initAdnlContext(config, DEFAULT_KEYSTORE_PATH)
                if (nativeContextPtr != 0L) {
                    runtimeConfig = transportConfig
                    startReceiverLoop()
                }
            }
            return nativeContextPtr
        }
    }

    external fun sendPacket(packet: ByteArray): Int

    /**
     * Быстрый путь отправки: передаем DirectByteBuffer напрямую в JNI.
     */
    fun sendPacket(packetBuffer: ByteBuffer, packetLength: Int): Int {
        if (!packetBuffer.isDirect) {
            val copied = ByteBuffer.allocateDirect(packetLength).order(ByteOrder.BIG_ENDIAN)
            val snapshot = packetBuffer.duplicate()
            val originalLimit = snapshot.limit()
            snapshot.limit(snapshot.position() + packetLength)
            copied.put(snapshot)
            copied.flip()
            snapshot.limit(originalLimit)
            return sendPacketDirect(copied, packetLength)
        }
        return sendPacketDirect(packetBuffer, packetLength)
    }

    /**
     * API по ТЗ: получение расшифрованного пакета.
     * Внутри используется DirectByteBuffer от native-слоя.
     */
    external fun receivePacket(): ByteArray?

    external fun sendPacketDirect(packetBuffer: ByteBuffer, packetLength: Int): Int

    external fun initAdnlContext(config: String, keystoreDir: String): Long

    external fun getLastNativeError(): String
    external fun configureAdnlPeer(
        egressAdnlAddress: String,
        clientPrivateKey: String,
        authToken: String,
        localUdpAdvertiseHost: String,
    ): Int

    fun registerStream(streamId: Int) {
        streamQueues.putIfAbsent(streamId, LinkedBlockingQueue())
    }

    fun unregisterStream(streamId: Int) {
        streamQueues.remove(streamId)
    }

    fun openStream(streamId: Int, host: String, port: Int): Int {
        if (DEBUG_VERBOSE) {
            Log.i(TAG, "openStream start host=$host port=$port streamId=$streamId")
        }
        val startedAt = System.currentTimeMillis()
        // Critical: ensure per-stream queue exists BEFORE sending OPEN, otherwise inbound OPEN_ACK can be dropped.
        registerStream(streamId)
        val token = runtimeConfig?.authToken.orEmpty()
        val hostBytes = host.toByteArray(StandardCharsets.UTF_8)
        val tokenBytes = token.toByteArray(StandardCharsets.UTF_8)
        if (hostBytes.size > 0xFFFF || tokenBytes.size > 0xFFFF || port !in 1..65535) {
            Log.w(TAG, "openStream rejected bad args host=$host port=$port streamId=$streamId")
            return -1
        }

        val frame = ByteBuffer.allocate(1 + 4 + 2 + hostBytes.size + 2 + 2 + tokenBytes.size)
            .order(ByteOrder.BIG_ENDIAN)
            .put(FrameType.OPEN.id.toByte())
            .putInt(streamId)
            .putShort(hostBytes.size.toShort())
            .put(hostBytes)
            .putShort(port.toShort())
            .putShort(tokenBytes.size.toShort())
            .put(tokenBytes)
            .array()
        if (DEBUG_VERBOSE) {
            Log.i(TAG, "dispatch OPEN over ADNL streamId=$streamId len=${frame.size}")
        }
        pendingOpen[streamId] = startedAt
        if (DEBUG_VERBOSE) {
            Log.i(TAG, "pendingOpen add streamId=$streamId")
        }
        jniCalls.incrementAndGet()
        val sendCode = sendPacket(frame)
        if (sendCode != 0) {
            Log.w(TAG, "openStream rejected sendPacket code=$sendCode host=$host port=$port streamId=$streamId")
            pendingOpen.remove(streamId)
            openFail.incrementAndGet()
            return sendCode
        }
        val deadlineMs = startedAt + OPEN_TIMEOUT_MS
        var lastUnexpected: FrameType? = null
        var closeSeenAtMs: Long? = null
        while (true) {
            val nowMs = System.currentTimeMillis()
            val remaining = deadlineMs - nowMs
            if (remaining <= 0) {
                Log.w(
                    TAG,
                    "pendingOpen timeout streamId=$streamId host=$host port=$port (${OPEN_TIMEOUT_MS}ms)${if (closeSeenAtMs != null) " saw_close_before_ack=true" else ""}",
                )
                runCatching {
                    closeStream(streamId)
                    if (DEBUG_VERBOSE) {
                        Log.i(TAG, "pendingOpen timeout close sent streamId=$streamId")
                    }
                }
                pendingOpen.remove(streamId)
                openTimeout.incrementAndGet()
                return -2
            }
            // If CLOSE has already arrived during OPEN, don't keep waiting full OPEN timeout.
            // Give a tiny reorder window, then fail fast.
            val waitMs = if (closeSeenAtMs != null) {
                val closeElapsed = nowMs - closeSeenAtMs!!
                val closeRemaining = OPEN_CLOSE_GRACE_MS - closeElapsed
                if (closeRemaining <= 0L) {
                    pendingOpen.remove(streamId)
                    openFail.incrementAndGet()
                    Log.w(
                        TAG,
                        "openStream closed during pendingOpen host=$host port=$port streamId=$streamId",
                    )
                    return -4
                }
                minOf(remaining, closeRemaining)
            } else {
                remaining
            }
            val await = awaitStreamFrame(streamId, waitMs)
            if (await.interrupted) {
                Log.w(TAG, "openStream interrupted host=$host port=$port streamId=$streamId")
                pendingOpen.remove(streamId)
                openFail.incrementAndGet()
                return -5
            }
            val response = await.frame ?: continue
            val rttMs = System.currentTimeMillis() - startedAt
            rttSumMs.addAndGet(rttMs)
            rttSamples.incrementAndGet()
            maybeLogMetrics()
            if (DEBUG_VERBOSE) {
                Log.i(TAG, "openStream answer received streamId=$streamId type=${response.type}")
            }
            when (response.type) {
                FrameType.OPEN -> {
                    pendingOpen.remove(streamId)
                    openOk.incrementAndGet()
                    if (DEBUG_VERBOSE) {
                        Log.i(TAG, "pendingOpen complete streamId=$streamId")
                        Log.i(TAG, "openStream ok host=$host port=$port streamId=$streamId")
                    }
                    return 0
                }
                FrameType.ERROR -> {
                    pendingOpen.remove(streamId)
                    openFail.incrementAndGet()
                    Log.w(TAG, "pendingOpen complete with ERROR streamId=$streamId")
                    Log.w(
                        TAG,
                        "openStream rejected/error host=$host port=$port streamId=$streamId msg=${response.errorMessage}",
                    )
                    return -3
                }
                FrameType.CLOSE -> {
                    // CLOSE during OPEN usually means immediate open rejection/teardown.
                    // Keep only a very short grace window for possible UDP reordering, then fail fast.
                    val now = System.currentTimeMillis()
                    if (closeSeenAtMs == null) {
                        closeSeenAtMs = now
                    } else if (now - closeSeenAtMs!! >= OPEN_CLOSE_GRACE_MS) {
                        pendingOpen.remove(streamId)
                        openFail.incrementAndGet()
                        Log.w(
                            TAG,
                            "openStream closed during pendingOpen host=$host port=$port streamId=$streamId",
                        )
                        return -4
                    }
                    lastUnexpected = FrameType.CLOSE
                }
                FrameType.DATA -> {
                    // Неожиданно получить DATA до OPEN_ACK возможно при переупорядочивании/переиспользовании streamId.
                    // Игнорируем и продолжаем ждать control frame.
                    lastUnexpected = FrameType.DATA
                }
                else -> {
                    lastUnexpected = response.type
                }
            }
            if (lastUnexpected != null && DEBUG_VERBOSE) {
                Log.i(TAG, "openStream keep-waiting streamId=$streamId unexpected=${lastUnexpected}")
            }
        }
    }

    fun sendStreamData(streamId: Int, payload: ByteArray): Int {
        if (payload.isEmpty()) {
            droppedEmptyFrames.incrementAndGet()
            maybeLogMetrics()
            return 0
        }
        // Stability mode: always send immediately (with segmentation), no micro-batching.
        return sendStreamDataImmediateSegmented(streamId, payload)
    }

    fun closeStream(streamId: Int): Int {
        val frame = ByteBuffer.allocate(1 + 4)
            .order(ByteOrder.BIG_ENDIAN)
            .put(FrameType.CLOSE.id.toByte())
            .putInt(streamId)
            .array()
        if (DEBUG_VERBOSE) {
            Log.i(TAG, "dispatch CLOSE over ADNL streamId=$streamId")
        }
        jniCalls.incrementAndGet()
        maybeLogMetrics()
        val code = sendPacket(frame)
        pendingOpen.remove(streamId)
        return code
    }

    private fun sendStreamDataImmediateSegmented(streamId: Int, payload: ByteArray): Int {
        val chunks = (payload.size + DATA_SEGMENT_SIZE - 1) / DATA_SEGMENT_SIZE
        if (DEBUG_VERBOSE && chunks > 1) {
            Log.i(TAG, "Android DATA segmented original=${payload.size} chunks=$chunks")
        }
        var offset = 0
        while (offset < payload.size) {
            val len = minOf(DATA_SEGMENT_SIZE, payload.size - offset)
            val frame = ByteBuffer.allocate(1 + 4 + 4 + len)
                .order(ByteOrder.BIG_ENDIAN)
                .put(FrameType.DATA.id.toByte())
                .putInt(streamId)
                .putInt(len)
                .put(payload, offset, len)
                .array()
            txFrames.incrementAndGet()
            txBytes.addAndGet(frame.size.toLong())
            txPayloadBytes.addAndGet(len.toLong())
            jniCalls.incrementAndGet()
            val code = sendPacket(frame)
            if (code != 0) {
                maybeLogMetrics()
                return code
            }
            offset += len
        }
        maybeLogMetrics()
        return 0
    }

    private fun formatHexHead(data: ByteArray, maxLen: Int = 48): String {
        val n = minOf(maxLen, data.size)
        val sb = StringBuilder(n * 2)
        for (i in 0 until n) {
            sb.append("%02x".format(data[i].toInt() and 0xff))
        }
        if (data.size > n) sb.append('…')
        return sb.toString()
    }

    fun awaitStreamFrame(streamId: Int, timeoutMs: Long): StreamAwaitResult {
        val queue = streamQueues[streamId] ?: return StreamAwaitResult(null, false)
        return try {
            val f = queue.poll(timeoutMs, TimeUnit.MILLISECONDS)
            StreamAwaitResult(f, false)
        } catch (_: InterruptedException) {
            // shutdownNow() etc. — propagate interrupt status, do not throw from worker threads.
            Thread.currentThread().interrupt()
            StreamAwaitResult(null, true)
        }
    }

    fun getRuntimeConfig(): TransportConfig? = runtimeConfig

    private fun startReceiverLoop() {
        if (!receiverRunning.compareAndSet(false, true)) return
        receiverExecutor.execute {
            while (receiverRunning.get()) {
                val frameBytes = runCatching { receivePacket() }.getOrNull()
                if (frameBytes == null || frameBytes.isEmpty()) {
                    Thread.sleep(RECEIVE_IDLE_SLEEP_MS)
                    continue
                }
                rxBytes.addAndGet(frameBytes.size.toLong())
                val decodedFrames = decodeInboundFrames(frameBytes)
                if (decodedFrames.isEmpty()) {
                    Log.w(TAG, "decodeInboundFrame failed len=${frameBytes.size}${if (DEBUG_VERBOSE) " hexHead=${formatHexHead(frameBytes)}" else ""}")
                    continue
                }
                for (decoded in decodedFrames) {
                    if (decoded.type == FrameType.OPEN && !pendingOpen.containsKey(decoded.streamId)) {
                        // OPEN_ACK arrived after timeout or for an already-closed stream: log once.
                        if (lateOpenAckLogged.putIfAbsent(decoded.streamId, true) == null) {
                            lateOpenAck.incrementAndGet()
                            Log.w(TAG, "late_open_ack streamId=${decoded.streamId}")
                        }
                    }
                    // Critical: never drop control frames due to missing queue. Create lazily.
                    val queue = streamQueues[decoded.streamId]
                        ?: if (decoded.type == FrameType.OPEN || decoded.type == FrameType.ERROR || decoded.type == FrameType.CLOSE) {
                            streamQueues.computeIfAbsent(decoded.streamId) { LinkedBlockingQueue() }
                        } else {
                            continue
                        }
                    queue.offer(decoded)
                }
                maybeLogStabilityCounters()
            }
        }
    }

    private fun decodeInboundFrames(bytes: ByteArray): List<InboundFrame> {
        val out = ArrayList<InboundFrame>(2)
        val buffer = ByteBuffer.wrap(bytes).order(ByteOrder.BIG_ENDIAN)
        while (buffer.remaining() >= 5) {
            val frameStart = buffer.position()
            val type = FrameType.fromId(buffer.get().toInt() and 0xFF)
            val streamId = buffer.int
            val decoded = when (type) {
                FrameType.UNKNOWN -> InboundFrame(type, streamId)
                FrameType.DATA -> {
                    if (buffer.remaining() < 4) return emptyList()
                    val payloadSize = buffer.int
                    if (payloadSize < 0 || buffer.remaining() < payloadSize) return emptyList()
                    val payload = ByteArray(payloadSize)
                    buffer.get(payload)
                    InboundFrame(type, streamId, payload = payload)
                }

                FrameType.CLOSE -> InboundFrame(type, streamId)

                FrameType.ERROR -> {
                    if (buffer.remaining() < 2) return emptyList()
                    val msgSize = buffer.short.toInt() and 0xFFFF
                    if (buffer.remaining() < msgSize) return emptyList()
                    val messageBytes = ByteArray(msgSize)
                    buffer.get(messageBytes)
                    InboundFrame(
                        type = type,
                        streamId = streamId,
                        errorMessage = String(messageBytes, StandardCharsets.UTF_8),
                    )
                }

                FrameType.OPEN -> InboundFrame(type, streamId)
            }
            val frameEnd = buffer.position()
            val raw = bytes.copyOfRange(frameStart, frameEnd)
            out.add(decoded.copy(rawFrame = raw))
        }
        return out
    }

    companion object {
        private const val TAG = "NativeTonTransport"
        private const val DEFAULT_KEYSTORE_PATH = "/data/user/0/com.example.dapf.tongate/files/ton_keystore"
        private const val RECEIVE_IDLE_SLEEP_MS = 10L
        /** Wait for OPEN reply from egress (TCP connect + auth on server can exceed 10s). */
        private const val OPEN_TIMEOUT_MS = 20_000L
        private const val METRICS_ENABLED = false
        private const val METRICS_LOG_WINDOW_MS = 5_000L
        private const val DEBUG_VERBOSE = false
        private const val DATA_SEGMENT_SIZE = 1200
        private const val OPEN_CLOSE_GRACE_MS = 250L
        private const val STABILITY_COUNTERS_ENABLED = true
        private const val STABILITY_COUNTERS_WINDOW_MS = 5_000L

        init {
            try {
                System.loadLibrary("ton_transport")
                Log.i(TAG, "ton_transport loaded successfully")
            } catch (t: Throwable) {
                Log.e(TAG, "Failed to load ton_transport: ${t.message}", t)
                throw t
            }
        }

        val instance: NativeTonTransport by lazy(LazyThreadSafetyMode.SYNCHRONIZED) {
            NativeTonTransport()
        }
    }
 
    private fun maybeLogStabilityCounters() {
        if (!STABILITY_COUNTERS_ENABLED) return
        val now = System.currentTimeMillis()
        val elapsed = now - metricsWindowStartMs
        if (elapsed < STABILITY_COUNTERS_WINDOW_MS) return
        val ok = openOk.getAndSet(0)
        val fail = openFail.getAndSet(0)
        val to = openTimeout.getAndSet(0)
        val txB = txBytes.getAndSet(0)
        val rxB = rxBytes.getAndSet(0)
        val active = streamQueues.size
        val late = lateOpenAck.getAndSet(0)
        Log.i(TAG, "counters open_ok=$ok open_fail=$fail open_timeout=$to tx_bytes=$txB rx_bytes=$rxB active_streams=$active late_open_ack=$late")
        metricsWindowStartMs = now
    }

    private fun maybeLogMetrics() {
        if (!METRICS_ENABLED) return
        val now = System.currentTimeMillis()
        val elapsed = now - metricsWindowStartMs
        if (elapsed < METRICS_LOG_WINDOW_MS) return
        val frames = txFrames.getAndSet(0)
        val bytes = txBytes.getAndSet(0)
        val payloadBytes = txPayloadBytes.getAndSet(0)
        val drops = droppedEmptyFrames.getAndSet(0)
        val calls = jniCalls.getAndSet(0)
        val rttTotal = rttSumMs.getAndSet(0)
        val rttCount = rttSamples.getAndSet(0)
        val avgPayload = if (frames > 0) payloadBytes.toDouble() / frames.toDouble() else 0.0
        val avgRtt = if (rttCount > 0) rttTotal.toDouble() / rttCount.toDouble() else -1.0
        val seconds = elapsed.coerceAtLeast(1L).toDouble() / 1000.0
        Log.i(
            TAG,
            "metrics tx_frames_per_sec=%.2f tx_bytes_per_sec=%.2f avg_payload_size=%.1f dropped_empty_frames=%d jni_calls_per_sec=%.2f adnl_rtt_ms=%.1f"
                .format(
                    frames / seconds,
                    bytes / seconds,
                    avgPayload,
                    drops,
                    calls / seconds,
                    avgRtt,
                ),
        )
        metricsWindowStartMs = now
    }
}
