package com.example.dapf.tongate.data.vpn

import android.util.Log
import com.example.dapf.tongate.data.native.NativeTonTransport
import java.io.InputStream
import java.io.OutputStream
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.Socket
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger

class LocalSocks5Server(
    private val bindAddress: String,
    private val bindPort: Int,
    private val nativeTransport: NativeTonTransport,
    private val shouldBlockTarget: (InetSocketAddress) -> Boolean
) {
    private val running = AtomicBoolean(false)
    private val pool = Executors.newCachedThreadPool()
    private val streamIdGenerator = AtomicInteger(1)
    private var serverSocket: ServerSocket? = null
    private val closeIssued = ConcurrentHashMap<Int, AtomicBoolean>()
    private val openFailLastLogMs = ConcurrentHashMap<String, Long>()

    fun start() {
        if (!running.compareAndSet(false, true)) return
        val server = ServerSocket()
        server.reuseAddress = true
        server.bind(InetSocketAddress(bindAddress, bindPort))
        serverSocket = server
        Log.i(TAG, "socks outbound host=$bindAddress port=$bindPort")

        pool.execute {
            while (running.get()) {
                val client = try {
                    server.accept()
                } catch (_: Throwable) {
                    null
                } ?: continue

                pool.execute { handleClient(client) }
            }
        }
    }

    fun stop() {
        running.set(false)
        runCatching { serverSocket?.close() }
        serverSocket = null
        pool.shutdownNow()
    }

    private fun handleClient(client: Socket) {
        client.use { c ->
            // Make SOCKS proxy snappy and avoid long half-open stalls.
            runCatching { c.tcpNoDelay = true }
            val input = c.getInputStream()
            val output = c.getOutputStream()
            if (!handshake(input, output)) {
                Log.w(TAG, "socks handshake failed remote=${c.remoteSocketAddress}")
                return@use
            }

            val target = parseConnectRequest(input, output) ?: return@use
            if (shouldBlockTarget(target)) {
                Log.w(TAG, "blocked outbound loop target=${target.hostString}:${target.port}")
                runCatching {
                    // host unreachable
                    output.write(byteArrayOf(0x05, 0x04, 0x00, 0x01, 0, 0, 0, 0, 0, 0))
                    output.flush()
                }
                return@use
            }
            val streamId = streamIdGenerator.getAndIncrement()
            Log.i(
                TAG,
                "openStream start (SOCKS) host=${target.hostString} port=${target.port} streamId=$streamId remote=${c.remoteSocketAddress}",
            )
            nativeTransport.registerStream(streamId)
            closeIssued[streamId] = AtomicBoolean(false)
            var openHandshakeDone = false
            var skipCloseStream = false
            try {
                val openCode = nativeTransport.openStream(streamId, target.hostString, target.port)
                if (openCode != 0) {
                    logOpenFailedDedup(streamId, target.hostString, target.port, openCode)
                    sendConnectFailure(output, socksRepForOpenFailure(openCode))
                    return@use
                }
                openHandshakeDone = true
                Log.i(TAG, "openStream success streamId=$streamId host=${target.hostString}:${target.port}")
                sendConnectSuccess(output)

                val sessionDone = AtomicBoolean(false)
                var upstreamThread: Thread? = null
                var downstreamThread: Thread? = null
                upstreamThread = Thread(
                    { pipeToTon(streamId, input, c, sessionDone) { downstreamThread?.interrupt() } },
                    "socks-up-$streamId",
                )
                downstreamThread = Thread(
                    { pipeFromTon(streamId, output, c, sessionDone) { upstreamThread?.interrupt() } },
                    "socks-down-$streamId",
                )
                upstreamThread.start()
                downstreamThread.start()
                try {
                    upstreamThread.join()
                    downstreamThread.join()
                } catch (ie: InterruptedException) {
                    Thread.currentThread().interrupt()
                    skipCloseStream = true
                    Log.w(
                        TAG,
                        "socks pipelines join interrupted streamId=$streamId host=${target.hostString}:${target.port} — skip ADNL CLOSE",
                        ie,
                    )
                    upstreamThread.interrupt()
                    downstreamThread.interrupt()
                    runCatching {
                        c.shutdownInput()
                        c.shutdownOutput()
                    }
                }
            } finally {
                when {
                    openHandshakeDone && !skipCloseStream -> {
                        Log.i(
                            TAG,
                            "socks teardown: ADNL closeStream after TCP/SOCKS finished streamId=$streamId host=${target.hostString}:${target.port}",
                        )
                        closeStreamOnce(streamId, "teardown")
                    }
                    !openHandshakeDone -> {
                        Log.i(TAG, "socks teardown: skip ADNL CLOSE (OPEN not completed) streamId=$streamId")
                    }
                    else -> {
                        Log.i(TAG, "socks teardown: skip ADNL CLOSE (join interrupted, stream may stay hot) streamId=$streamId")
                    }
                }
                closeIssued.remove(streamId)
                nativeTransport.unregisterStream(streamId)
            }
            Log.i(TAG, "socks socket closed handler exit remote=${c.remoteSocketAddress}")
        }
    }

    private fun pipeToTon(streamId: Int, input: InputStream) {
        val buffer = ByteArray(8192)
        while (running.get()) {
            if (Thread.currentThread().isInterrupted) {
                Log.w(TAG, "pipeToTon interrupted streamId=$streamId")
                return
            }
            val read = try {
                input.read(buffer)
            } catch (_: Throwable) {
                -1
            }
            if (read <= 0) break
            val payload = if (read == buffer.size) buffer else buffer.copyOf(read)
            if (payload.isEmpty()) continue
            if (DEBUG_VERBOSE) {
                Log.d(TAG, "DATA from SOCKS client streamId=$streamId size=${payload.size}")
            }
            val sendStartedNs = System.nanoTime()
            val sendCode = nativeTransport.sendStreamData(streamId, payload)
            val delayMs = (System.nanoTime() - sendStartedNs) / 1_000_000L
            if (DEBUG_VERBOSE) {
                Log.d(TAG, "DATA immediate send streamId=$streamId size=${payload.size}")
                Log.d(TAG, "DATA flush delay ms=$delayMs streamId=$streamId size=${payload.size}")
            }
            if (delayMs > SEND_DELAY_WARN_MS) {
                Log.w(TAG, "DATA flush delay is high streamId=$streamId size=${payload.size} delayMs=$delayMs")
            }
            if (sendCode != 0) {
                Log.w(TAG, "stream data send failed: code=$sendCode streamId=$streamId")
                return
            }
        }
    }

    private fun pipeToTon(
        streamId: Int,
        input: InputStream,
        socket: Socket,
        sessionDone: AtomicBoolean,
        interruptOtherSide: () -> Unit,
    ) {
        try {
            pipeToTon(streamId, input)
        } finally {
            if (sessionDone.compareAndSet(false, true)) {
                // Upstream ended (client closed or error) -> stop downstream and unblock reads.
                runCatching { interruptOtherSide() }
                runCatching { socket.shutdownInput() }
                runCatching { socket.shutdownOutput() }
                closeStreamOnce(streamId, "socks upstream end")
            }
        }
    }

    private fun pipeFromTon(streamId: Int, output: OutputStream) {
        while (running.get()) {
            if (Thread.currentThread().isInterrupted) {
                Log.w(TAG, "pipeFromTon interrupted streamId=$streamId")
                return
            }
            val ar = nativeTransport.awaitStreamFrame(streamId, STREAM_QUERY_AWAIT_MS)
            if (ar.interrupted) {
                Log.w(TAG, "pipeFromTon queue poll interrupted streamId=$streamId")
                return
            }
            val frame = ar.frame
            if (frame == null) {
                continue
            }
            when (frame.type) {
                NativeTonTransport.FrameType.DATA -> {
                    if (DEBUG_VERBOSE) {
                        Log.d(TAG, "DATA from ADNL streamId=$streamId payloadLen=${frame.payload.size}")
                    }
                    if (frame.payload.isNotEmpty()) {
                        try {
                            output.write(frame.payload)
                            if (DEBUG_VERBOSE) {
                                Log.d(TAG, "wrote to SOCKS socket streamId=$streamId bytes=${frame.payload.size}")
                            }
                        } catch (t: Throwable) {
                            Log.w(
                                TAG,
                                "write to SOCKS output failed streamId=$streamId; mark closed and stop downstream (${t.javaClass.simpleName}: ${t.message})",
                            )
                            closeStreamOnce(streamId, "socks output failed")
                            return
                        }
                    }
                }

                NativeTonTransport.FrameType.CLOSE -> {
                    Log.i(TAG, "ADNL CLOSE frame from server streamId=$streamId — stop downstream")
                    return
                }

                NativeTonTransport.FrameType.ERROR -> {
                    Log.w(TAG, "stream error streamId=$streamId: ${frame.errorMessage ?: "unknown"}")
                    return
                }

                NativeTonTransport.FrameType.OPEN -> {
                    // Duplicate OPEN in queue after handshake — do not stop pipe (was stopping TLS cold start).
                    Log.d(TAG, "ignore spurious OPEN frame in data phase streamId=$streamId")
                }

                NativeTonTransport.FrameType.UNKNOWN -> {
                    Log.w(TAG, "unknown ADNL frame type in data phase streamId=$streamId")
                }
            }
        }
    }

    private fun pipeFromTon(
        streamId: Int,
        output: OutputStream,
        socket: Socket,
        sessionDone: AtomicBoolean,
        interruptOtherSide: () -> Unit,
    ) {
        try {
            pipeFromTon(streamId, output)
        } finally {
            if (sessionDone.compareAndSet(false, true)) {
                // Downstream ended (server closed or error) -> stop upstream and unblock writes.
                runCatching { interruptOtherSide() }
                runCatching { socket.shutdownInput() }
                runCatching { socket.shutdownOutput() }
                closeStreamOnce(streamId, "socks downstream end")
            }
        }
    }

    /** SOCKS5 REP byte (second octet of reply). */
    private fun socksRepForOpenFailure(openCode: Int): Byte {
        return when (openCode) {
            -2 -> 0x06 // TTL expired — used for open timeout
            -3 -> 0x05 // connection refused — peer returned ERROR frame
            -5 -> 0x01 // general failure — interrupted wait
            else -> 0x01
        }.toByte()
    }

    private fun sendConnectFailure(output: OutputStream, rep: Byte) {
        val response = ByteArray(10)
        response[0] = 0x05
        response[1] = rep
        response[2] = 0x00
        response[3] = 0x01
        val z = byteArrayOf(0, 0, 0, 0)
        System.arraycopy(z, 0, response, 4, 4)
        response[8] = 0
        response[9] = 0
        runCatching {
            output.write(response)
            output.flush()
        }
    }

    private fun handshake(input: InputStream, output: OutputStream): Boolean {
        val version = input.read()
        if (version != 0x05) return false
        val methodsCount = input.read()
        if (methodsCount <= 0) return false
        val methods = ByteArray(methodsCount)
        if (!readFully(input, methods)) return false
        if (!methods.contains(0x00.toByte())) {
            output.write(byteArrayOf(0x05, 0xFF.toByte()))
            output.flush()
            return false
        }
        output.write(byteArrayOf(0x05, 0x00))
        output.flush()
        return true
    }

    private fun parseConnectRequest(input: InputStream, output: OutputStream): InetSocketAddress? {
        val header = ByteArray(4)
        if (!readFully(input, header)) return null
        val ver = header[0].toInt() and 0xFF
        val cmd = header[1].toInt() and 0xFF
        val atyp = header[3].toInt() and 0xFF
        if (ver != 0x05 || cmd != 0x01) {
            output.write(byteArrayOf(0x05, 0x07, 0x00, 0x01, 0, 0, 0, 0, 0, 0))
            output.flush()
            return null
        }

        val address = when (atyp) {
            0x01 -> {
                val ipv4 = ByteArray(4)
                if (!readFully(input, ipv4)) return null
                InetAddress.getByAddress(ipv4).hostAddress ?: return null
            }
            0x03 -> {
                val len = input.read()
                if (len <= 0) return null
                val domainBytes = ByteArray(len)
                if (!readFully(input, domainBytes)) return null
                String(domainBytes, Charsets.US_ASCII)
            }
            0x04 -> {
                val ipv6 = ByteArray(16)
                if (!readFully(input, ipv6)) return null
                InetAddress.getByAddress(ipv6).hostAddress ?: return null
            }
            else -> return null
        }

        val portBytes = ByteArray(2)
        if (!readFully(input, portBytes)) return null
        val port = ((portBytes[0].toInt() and 0xFF) shl 8) or (portBytes[1].toInt() and 0xFF)
        return InetSocketAddress(address, port)
    }

    private fun sendConnectSuccess(output: OutputStream) {
        val response = ByteArray(10)
        response[0] = 0x05
        response[1] = 0x00
        response[2] = 0x00
        response[3] = 0x01
        val ipv4 = byteArrayOf(0, 0, 0, 0)
        System.arraycopy(ipv4, 0, response, 4, 4)
        // For CONNECT, many clients ignore BND.ADDR/BND.PORT, but strict ones may validate.
        // We don't have a real TCP connect socket here, so return 0.0.0.0:0.
        val port = 0
        response[8] = ((port ushr 8) and 0xFF).toByte()
        response[9] = (port and 0xFF).toByte()
        output.write(response)
        output.flush()
    }

    private fun readFully(input: InputStream, buffer: ByteArray): Boolean {
        var offset = 0
        while (offset < buffer.size) {
            val read = input.read(buffer, offset, buffer.size - offset)
            if (read <= 0) return false
            offset += read
        }
        return true
    }

    private fun closeStreamOnce(streamId: Int, reason: String) {
        val guard = closeIssued.computeIfAbsent(streamId) { AtomicBoolean(false) }
        if (guard.compareAndSet(false, true)) {
            Log.i(TAG, "closeStream once streamId=$streamId reason=$reason")
            runCatching { nativeTransport.closeStream(streamId) }
        }
    }

    private fun logOpenFailedDedup(streamId: Int, host: String, port: Int, openCode: Int) {
        val reason = when (openCode) {
            -2 -> "timeout"
            -3 -> "egress_error"
            -5 -> "interrupted"
            else -> "code_$openCode"
        }
        val key = "$host:$port:$reason"
        val now = System.currentTimeMillis()
        val prev = openFailLastLogMs[key]
        if (prev == null || now - prev >= OPEN_FAIL_DEDUP_MS) {
            openFailLastLogMs[key] = now
            Log.w(TAG, "OPEN failed streamId=$streamId host=$host port=$port reason=$reason")
        }
    }

    companion object {
        private const val TAG = "LocalSocks5Server"
        private const val DEBUG_VERBOSE = false
        private const val STREAM_QUERY_AWAIT_MS = 30_000L
        private const val SEND_DELAY_WARN_MS = 10L
        private const val OPEN_FAIL_DEDUP_MS = 5_000L
    }
}
