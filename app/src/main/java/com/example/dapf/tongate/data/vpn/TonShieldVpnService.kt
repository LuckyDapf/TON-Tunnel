package com.example.dapf.tongate.data.vpn

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.content.Intent
import android.content.pm.PackageManager
import android.net.VpnService
import android.os.Build
import android.os.IBinder
import android.os.ParcelFileDescriptor
import android.util.Log
import androidx.core.app.NotificationCompat
import com.example.dapf.tongate.R
import com.example.dapf.tongate.data.native.NativeTonTransport
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import java.io.File
import java.io.FileInputStream
import java.io.FileOutputStream
import java.io.IOException
import java.io.InputStream
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.Inet4Address
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.NetworkInterface
import java.net.Socket
import java.net.SocketTimeoutException
import java.nio.ByteBuffer
import javax.net.ssl.SSLSocket
import javax.net.ssl.SSLSocketFactory
import java.util.Collections
import java.util.concurrent.atomic.AtomicInteger
import org.json.JSONObject

/**
 * Базовый скелет VPN-ядра:
 * - поднимает TUN-интерфейс;
 * - запускает неблокирующий цикл чтения IP-пакетов;
 * - готов к дальнейшей интеграции DNS-фильтра и TON/ADNL транспорта.
 */
class TonShieldVpnService : VpnService() {

    private val ioDispatcher: CoroutineDispatcher = Dispatchers.IO
    private var serviceScope = createServiceScope()

    private var vpnInterface: ParcelFileDescriptor? = null
    private var packetLoopJob: Job? = null
    private var dnsLoopJob: Job? = null
    private var inboundTonLoopJob: Job? = null
    private var tun2SocksJob: Job? = null
    private var dnsSocket: DatagramSocket? = null
    private var localSocks5Server: LocalSocks5Server? = null
    private var secureDnsGateway: SecureDnsGateway? = null
    private val nativeTransport = NativeTonTransport.instance
    private val blockedThreatsCount = AtomicInteger(0)
    private var vpnEnabled = false
    private var nativeInitialized = false
    private var secureDnsWarningSent = false
    private var selfBypassEnabled = false

    /**
     * Потокобезопасный обработчик DNS-запросов.
     * Blacklist хранится как неизменяемый Set, чтобы чтение из разных корутин было безопасным.
     */
    private val dnsPacketHandler = DnsPacketHandler(
        blockedDomains = setOf(
            "ads.example.com",
            "tracker.example.net",
            "telemetry.example.org"
        )
    )

    override fun onBind(intent: Intent): IBinder? {
        return super.onBind(intent)
    }

    override fun onCreate() {
        super.onCreate()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP_VPN) {
            stopVpnService()
            return START_NOT_STICKY
        }

        // Не вызываем VpnService.prepare() здесь: разрешение уже запрашивается в MainActivity.
        // Повторный prepare из контекста сервиса на некоторых прошивках (напр. MIUI) даёт
        // AppOps SecurityException «package does not belong to uid» при том же приложении.
        // Если разрешения нет, Builder.establish() вернёт null — см. establishTunInterface().

        if (vpnEnabled) {
            emitStatsUpdate()
            return START_STICKY
        }

        startForeground(
            VPN_NOTIFICATION_ID,
            buildForegroundNotification()
        )

        if (packetLoopJob?.isActive != true) {
            packetLoopJob = serviceScope.launch {
                runVpnTunnelLoop()
            }
        }
        if (DNS_FILTER_ENABLED && dnsLoopJob?.isActive != true) {
            dnsLoopJob = serviceScope.launch {
                runLocalDnsServerLoop()
            }
        }

        vpnEnabled = true
        emitStatsUpdate()
        return START_STICKY
    }

    override fun onDestroy() {
        stopVpnService()
        super.onDestroy()
    }

    private fun stopVpnService() {
        packetLoopJob?.cancel()
        packetLoopJob = null
        dnsLoopJob?.cancel()
        dnsLoopJob = null
        inboundTonLoopJob?.cancel()
        inboundTonLoopJob = null
        tun2SocksJob?.cancel()
        tun2SocksJob = null
        runCatching { Tun2SocksNative.stopTun2SocksNative() }
        secureDnsGateway?.stop()
        secureDnsGateway = null
        localSocks5Server?.stop()
        localSocks5Server = null
        dnsSocket?.close()
        dnsSocket = null
        vpnInterface?.close()
        vpnInterface = null
        stopForeground(STOP_FOREGROUND_REMOVE)
        stopSelf()
        vpnEnabled = false
        emitStatsUpdate()
        serviceScope.cancel()
        serviceScope = createServiceScope()
    }

    /**
     * Конфигурирует виртуальный сетевой интерфейс TUN.
     * Важно: маршрут 0.0.0.0/0 принудительно отправляет весь IPv4-трафик в VPN-движок.
     */
    private fun establishTunInterface(): ParcelFileDescriptor? {
        return try {
            val builder = Builder()
                .setSession(VPN_SESSION_NAME)
                .setMtu(VPN_MTU)
                .addAddress(VPN_IPV4_ADDRESS, VPN_IPV4_PREFIX)
                .addRoute("0.0.0.0", 0)
                .addDnsServer(VPN_DNS_SERVER)

            selfBypassEnabled = runCatching {
                builder.addDisallowedApplication(packageName)
                true
            }.getOrElse { error ->
                val isNameNotFound = error is PackageManager.NameNotFoundException
                if (!isNameNotFound) {
                    Log.w(TAG, "Failed to add disallowed app for self-bypass: ${error.message}")
                }
                false
            }
            Log.i(TAG, "VPN self-bypass enabled=$selfBypassEnabled")

            builder.establish()
        } catch (t: Throwable) {
            // Не даем сервису упасть при невалидном сетевом конфиге.
            Log.e(TAG, "Failed to establish TUN interface", t)
            sendVpnErrorBroadcast(t.message ?: "Не удалось инициализировать VPN-интерфейс.")
            null
        }
    }

    /**
     * Основной цикл чтения пакетов из TUN.
     * Используется direct ByteBuffer, чтобы минимизировать количество копирований между JVM и native-слоем.
     */
    private suspend fun runVpnTunnelLoop() {
        if (!nativeInitialized) {
            nativeInitialized = ensureNativeInitialized()
            if (!nativeInitialized) {
                Log.e(TAG, "Native init failed in runVpnTunnelLoop; detailed error was sent earlier")
                stopSelf()
                return
            }
        }

        val tun = establishTunInterface() ?: return
        vpnInterface = tun

        val input = FileInputStream(tun.fileDescriptor)
        val output = FileOutputStream(tun.fileDescriptor)
        val packetBuffer = ByteBuffer.allocateDirect(VPN_MTU)
        val outputWriteLock = Any()

        // В режиме tun2socks входящий поток из tonlib не должен писать в TUN,
        // иначе можно получить конфликт с TCP forwarding.
        inboundTonLoopJob?.cancel()
        inboundTonLoopJob = null

        secureDnsGateway = SecureDnsGateway(
            bindAddress = LOCALHOST,
            bindPort = DNS_GW_PORT,
            resolver = { query ->
                val response = resolveDnsSecurely(query)
                if (response != null) {
                    Log.i(VPN_TRAFFIC_TAG, "secure DNS handled")
                } else {
                    Log.w(VPN_TRAFFIC_TAG, "DNS query failed")
                }
                response
            }
        ).also { it.start() }

        val socksServer = runCatching {
            LocalSocks5Server(
                bindAddress = LOCALHOST,
                bindPort = LOCAL_SOCKS_PORT,
                nativeTransport = nativeTransport,
                shouldBlockTarget = { target -> isLoopTarget(target) }
            ).also { it.start() }
        }.getOrElse { error ->
            Log.e(VPN_TRAFFIC_TAG, "No SOCKS outbound configured: ${error.message}")
            sendVpnErrorBroadcast("No SOCKS outbound configured")
            stopSelf()
            return
        }
        localSocks5Server = socksServer

        if (!verifyLocalSocksServerReady()) {
            Log.e(VPN_TRAFFIC_TAG, "No SOCKS outbound configured")
            sendVpnErrorBroadcast("No SOCKS outbound configured")
            stopSelf()
            return
        }

        tun2SocksJob = serviceScope.launch {
            val args = arrayOf(
                "badvpn-tun2socks",
                "--logger", "stdout",
                "--loglevel", "3",
                "--tunfd", tun.fd.toString(),
                "--tunmtu", VPN_MTU.toString(),
                "--netif-ipaddr", VPN_IPV4_ADDRESS,
                "--netif-netmask", "255.255.255.0",
                "--socks-server-addr", "$LOCALHOST:$LOCAL_SOCKS_PORT",
                "--dnsgw", "$LOCALHOST:$DNS_GW_PORT"
            )
            Log.i(VPN_TRAFFIC_TAG, "tun2socks args=${args.joinToString(" ")}")
            Log.i(VPN_TRAFFIC_TAG, "tun2socks started")
            Log.i(VPN_TRAFFIC_TAG, "TCP forwarding active")
            val code = Tun2SocksNative.startTun2SocksNative(args)
            Log.i(VPN_TRAFFIC_TAG, "tun2socks stopped, exitCode=$code")
            if (code != 0 && serviceScope.isActive) {
                sendVpnErrorBroadcast("tun2socks failed: code=$code")
            }
        }

        try {
            while (serviceScope.isActive) {
                // tun2socks читает TUN напрямую через fd. Корутина держит lifecycle сервиса.
                delay(500)
            }
        } catch (e: IOException) {
            // Дескриптор TUN может закрыться во время остановки сервиса — это штатная ситуация.
            val isDescriptorClosed = e.message?.contains("Bad file descriptor", ignoreCase = true) == true
            if (isDescriptorClosed) {
                Log.d(TAG, "TUN descriptor closed, finishing loop")
            } else if (serviceScope.isActive) {
                Log.e(TAG, "TUN read loop failed unexpectedly", e)
                sendVpnErrorBroadcast("Ошибка чтения VPN интерфейса: ${e.message ?: "io error"}")
            } else {
                Log.d(TAG, "TUN loop stopped after descriptor close")
            }
        } finally {
            runCatching { Tun2SocksNative.stopTun2SocksNative() }
            secureDnsGateway?.stop()
            secureDnsGateway = null
            localSocks5Server?.stop()
            localSocks5Server = null
            tun2SocksJob?.cancel()
            tun2SocksJob = null
            runCatching { input.close() }
            runCatching { output.close() }
        }
    }

    /**
     * Цикл приема из TON:
     * - читает пакеты из JNI;
     * - пишет их в tun0 через тот же FileDescriptor;
     * - при пустом результате делает короткую паузу, чтобы не выжигать CPU.
     */
    private suspend fun runInboundTonLoop(
        tunOutput: FileOutputStream,
        outputWriteLock: Any
    ) {
        while (serviceScope.isActive) {
            val inboundPacket = nativeTransport.receivePacket()
            if (inboundPacket == null || inboundPacket.isEmpty()) {
                delay(TON_INBOUND_IDLE_DELAY_MS)
                continue
            }

            if (!looksLikeIpPacket(inboundPacket)) {
                // tonlib часто отдает JSON-апдейты. В TUN можно писать только raw IP frame.
                continue
            }
            Log.d(VPN_TRAFFIC_TAG, "IN: Received from JNI ${inboundPacket.size} bytes")

            try {
                synchronized(outputWriteLock) {
                    tunOutput.write(inboundPacket)
                    tunOutput.flush()
                }
            } catch (e: IOException) {
                Log.e(VPN_TUN_WRITE_TAG, "Ошибка записи в интерфейс", e)
                if (serviceScope.isActive) {
                    sendVpnErrorBroadcast("Ошибка записи VPN интерфейса: ${e.message ?: "io error"}")
                }
                break
            }
        }
    }

    /**
     * Минимальная валидация сетевого пакета перед записью в TUN.
     * Проверяем версию IP в старших 4 битах первого байта.
     */
    private fun looksLikeIpPacket(packet: ByteArray): Boolean {
        if (packet.isEmpty()) return false
        val version = (packet[0].toInt() ushr 4) and 0x0F
        return version == IPV4_VERSION || version == IPV6_VERSION
    }

    /**
     * Локальный UDP DNS-сервер:
     * - принимает DNS query от приложений;
     * - мгновенно отвечает NXDOMAIN для blacklist;
     * - разрешенные запросы помечает как TODO для дальнейшей отправки в TON transport.
     */
    private suspend fun runLocalDnsServerLoop() {
        if (!nativeInitialized) {
            nativeInitialized = ensureNativeInitialized()
            if (!nativeInitialized) {
                Log.e(TAG, "Native init failed in runLocalDnsServerLoop; detailed error was sent earlier")
                return
            }
        }

        // DNS-сервер слушает тот же адрес, который объявлен в Builder.addDnsServer(...).
        // Это устраняет рассинхрон и повышает шанс, что системные DNS-запросы попадут в фильтр.
        val localAddress = InetAddress.getByName(VPN_DNS_SERVER)
        val socket = DatagramSocket(null).apply {
            reuseAddress = true
            soTimeout = DNS_SOCKET_TIMEOUT_MS
            bind(InetSocketAddress(localAddress, DNS_LOCAL_PORT))
        }
        dnsSocket = socket

        // DNS-сокет не должен обходить VPN через TUN, иначе можно словить петлю маршрутизации.
        protect(socket)

        val receiveBuffer = ByteArray(DNS_MAX_PACKET_SIZE)
        val receivePacket = DatagramPacket(receiveBuffer, receiveBuffer.size)

        while (serviceScope.isActive) {
            try {
                receivePacket.length = receiveBuffer.size
                socket.receive(receivePacket)

                val handlingResult = dnsPacketHandler.handleQuery(
                    packetBytes = receivePacket.data,
                    packetLength = receivePacket.length
                )

                when (handlingResult) {
                    is DnsPacketHandler.Result.Blocked -> {
                        val responsePacket = DatagramPacket(
                            handlingResult.responseBytes,
                            handlingResult.responseLength,
                            receivePacket.address,
                            receivePacket.port
                        )
                        socket.send(responsePacket)
                        blockedThreatsCount.incrementAndGet()
                        emitStatsUpdate()
                    }

                    is DnsPacketHandler.Result.Allowed -> {
                        // Privacy-first режим: не отправляем DNS наружу как обычный UDP/53.
                        val secureResponse = resolveDnsSecurely(handlingResult.originalPacket)
                        if (secureResponse != null) {
                            val responsePacket = DatagramPacket(
                                secureResponse,
                                secureResponse.size,
                                receivePacket.address,
                                receivePacket.port
                            )
                            socket.send(responsePacket)
                            Log.i(VPN_TRAFFIC_TAG, "DNS query handled securely")
                        } else {
                            Log.w(VPN_TRAFFIC_TAG, "DNS query failed")
                            if (!secureDnsWarningSent) {
                                sendVpnErrorBroadcast("Secure DNS not ready")
                                secureDnsWarningSent = true
                            }
                        }
                    }

                    DnsPacketHandler.Result.Invalid -> {
                        // Некорректный DNS-пакет игнорируем без ответа.
                    }
                }
            } catch (_: SocketTimeoutException) {
                // Таймаут используется как "heartbeat", чтобы цикл регулярно проверял состояние корутины.
            }
        }
    }

    private fun buildForegroundNotification(): Notification {
        createNotificationChannelIfNeeded()

        return NotificationCompat.Builder(this, VPN_CHANNEL_ID)
            .setSmallIcon(R.mipmap.ic_launcher)
            .setContentTitle(getString(R.string.app_name))
            .setContentText("TON Gate VPN активен")
            .setPriority(NotificationCompat.PRIORITY_LOW)
            .setOngoing(true) // Нельзя смахнуть, пока сервис работает.
            .build()
    }

    private fun createNotificationChannelIfNeeded() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return

        val channel = NotificationChannel(
            VPN_CHANNEL_ID,
            "TON Gate VPN",
            NotificationManager.IMPORTANCE_LOW
        ).apply {
            description = "Служебный канал для VPN-сервиса"
            setShowBadge(false)
        }

        val notificationManager = getSystemService(NotificationManager::class.java)
        notificationManager.createNotificationChannel(channel)
    }

    /**
     * Передача оперативной статистики в UI:
     * - текущее состояние VPN;
     * - количество заблокированных DNS-угроз.
     */
    private fun emitStatsUpdate() {
        val statsIntent = Intent(ACTION_VPN_STATS).apply {
            setPackage(packageName)
            putExtra(EXTRA_VPN_ENABLED, vpnEnabled)
            putExtra(EXTRA_BLOCKED_COUNT, blockedThreatsCount.get())
        }
        sendBroadcast(statsIntent)
    }

    /**
     * Широковещательная отправка текстовой ошибки для UI.
     */
    private fun sendVpnErrorBroadcast(message: String) {
        val errorIntent = Intent(ACTION_VPN_ERROR).apply {
            setPackage(packageName)
            putExtra(EXTRA_ERROR_MESSAGE, message)
        }
        sendBroadcast(errorIntent)
    }

    /**
     * Инициализация JNI/ADNL-контекста с базовой валидацией конфигурации.
     * Если контекст не создан или конфиг пустой, считаем инициализацию проваленной.
     */
    private fun ensureNativeInitialized(): Boolean {
        val configJson = loadDefaultGlobalConfig()
        if (configJson.isBlank()) {
            Log.e(TAG, "ADNL config is empty, native transport cannot be initialized.")
            return false
        }

        val keystoreDir = File(filesDir, "ton_keystore")
        if (!keystoreDir.exists() && !keystoreDir.mkdirs()) {
            Log.e(TAG, "Failed to create TON keystore directory: ${keystoreDir.absolutePath}")
            sendVpnErrorBroadcast("Не удалось создать директорию ton_keystore")
            return false
        }

        return try {
            Log.i(TAG, "tonlib init started")
            val transportConfig = loadAdnlTransportConfig()
            if (transportConfig.egressAdnlAddress.isBlank() ||
                transportConfig.clientPrivateKey.isBlank() ||
                transportConfig.authToken.isBlank()
            ) {
                sendVpnErrorBroadcast("ADNL transport config is incomplete")
                return false
            }
            val egressValidationError = validateEgressAdnlAddress(transportConfig.egressAdnlAddress)
            if (egressValidationError != null) {
                Log.e(TAG, "Invalid egress_adnl_address: $egressValidationError")
                sendVpnErrorBroadcast("Неверный egress_adnl_address: $egressValidationError")
                return false
            }
            val advertiseError = validateLocalUdpAdvertiseHost(transportConfig.localUdpAdvertiseHost)
            if (advertiseError != null) {
                Log.e(TAG, "Invalid local_udp advertise: $advertiseError")
                sendVpnErrorBroadcast(advertiseError)
                return false
            }
            logAdvertiseDecision(transportConfig.egressAdnlAddress, transportConfig.localUdpAdvertiseHost)
            val contextPtr = nativeTransport.initialize(configJson, transportConfig)
            val initialized = contextPtr != 0L
            if (!initialized) {
                val nativeError = nativeTransport.getLastNativeError()
                Log.e(TAG, "Native transport returned null context pointer. $nativeError")
                val userError = if (nativeError.contains("libton_adnl_client.so", ignoreCase = true)) {
                    "Не найден libton_adnl_client.so (ожидается в app/src/main/jniLibs/arm64-v8a/)"
                } else if (nativeError.isNotBlank()) {
                    nativeError
                } else {
                    "Ошибка инициализации TON transport"
                }
                sendVpnErrorBroadcast(
                    userError
                )
            } else {
                Log.i(TAG, "tonlib init success")
            }
            initialized
        } catch (t: Throwable) {
            Log.e(TAG, "Native transport initialization failed.", t)
            sendVpnErrorBroadcast("Сбой инициализации native: ${t.message ?: "unknown"}")
            false
        }
    }

    /**
     * Загружает global-config.json из filesDir.
     * Если файла нет, копирует его из res/raw/global_config.json.
     */
    private fun loadDefaultGlobalConfig(): String {
        return try {
            val configFile = File(filesDir, "global-config.json")
            if (!configFile.exists() || configFile.length() == 0L) {
                resources.openRawResource(R.raw.global_config).use { input ->
                    configFile.outputStream().use { output ->
                        input.copyTo(output)
                    }
                }
            }
            configFile.readText()
        } catch (t: Throwable) {
            Log.e(TAG, "Failed to load global config from resources.", t)
            ""
        }
    }

    private fun loadAdnlTransportConfig(): NativeTonTransport.TransportConfig {
        return try {
            val configFile = File(filesDir, "adnl-transport-config.json")
            if (!configFile.exists() || configFile.length() == 0L) {
                resources.openRawResource(R.raw.adnl_transport_config).use { input ->
                    configFile.outputStream().use { output ->
                        input.copyTo(output)
                    }
                }
            }
            val json = JSONObject(configFile.readText())
            val egress = json.optString("egress_adnl_address", "")
            val fromJson = json.optString("local_udp_advertise_host", "").trim()
            val resolvedAdvertise = resolveLocalUdpAdvertiseHost(fromJson, egress)
            NativeTonTransport.TransportConfig(
                egressAdnlAddress = egress,
                clientPrivateKey = json.optString("client_private_key", ""),
                authToken = json.optString("auth_token", ""),
                localUdpAdvertiseHost = resolvedAdvertise,
            )
        } catch (t: Throwable) {
            Log.e(TAG, "Failed to load ADNL transport config", t)
            NativeTonTransport.TransportConfig("", "", "", "")
        }
    }

    /**
     * IPv4 that peers must use to reach this device on the ADNL UDP listen port.
     * Prefer explicit JSON; else same /24 as numeric private egress host; else no-advertise.
     */
    private fun resolveLocalUdpAdvertiseHost(fromJson: String, egressAdnlAddress: String): String {
        val manual = fromJson.trim()
        if (manual.isNotEmpty()) {
            val manualInet = runCatching { InetAddress.getByName(manual) }.getOrNull() as? Inet4Address
            if (manualInet != null && !manualInet.isSiteLocalAddress) {
                Log.w(TAG, "Ignoring public local_udp_advertise_host=$manual on Android; NAT may rewrite UDP source port")
                return ""
            }
            return manual
        }
        val lanMatched = inferLanIpv4MatchingEgressSubnet(egressAdnlAddress)
        if (!lanMatched.isNullOrBlank()) {
            return lanMatched
        }
        Log.i(TAG, "local_udp_advertise_host is empty and no LAN /24 match; using no-advertise")
        return ""
    }


    private fun inferLanIpv4MatchingEgressSubnet(egressAdnl: String): String? {
        val at = egressAdnl.indexOf('@')
        if (at <= 0 || at >= egressAdnl.length - 1) return null
        val hostPort = parseHostPort(egressAdnl.substring(at + 1)) ?: return null
        val egressHost = hostPort.first
        val egressInet = runCatching { InetAddress.getByName(egressHost) }.getOrNull() as? Inet4Address ?: return null
        if (!egressInet.isSiteLocalAddress) return null
        val egressBytes = egressInet.address
        val nis = Collections.list(NetworkInterface.getNetworkInterfaces())
        for (ni in nis) {
            if (!ni.isUp || ni.isLoopback) continue
            for (addr in Collections.list(ni.inetAddresses)) {
                if (addr !is Inet4Address || addr.isLoopbackAddress) continue
                val ab = addr.address ?: continue
                if (ab.size < 4) continue
                if (ab[0] == egressBytes[0] && ab[1] == egressBytes[1] && ab[2] == egressBytes[2]) {
                    return addr.hostAddress
                }
            }
        }
        return null
    }

    private fun validateLocalUdpAdvertiseHost(host: String): String? {
        val t = host.trim()
        if (t.isEmpty()) {
            return null
        }
        if (t == "0.0.0.0" || t == "127.0.0.1") {
            return "local_udp_advertise_host не может быть $t"
        }
        val inet = runCatching { InetAddress.getByName(t) }.getOrElse { return "local_udp_advertise_host: неверный IP: $t" }
        if (inet !is Inet4Address) return "local_udp_advertise_host должен быть IPv4 (сейчас: $t)"
        return null
    }

    /**
     * Формат egress_adnl_address:
     *   <pubkey_hex>@<host:port>
     * где:
     *   - pubkey_hex: 64 hex-символа (32 байта ed25519 pubkey)
     *   - host: IPv4, домен, либо [IPv6]
     *   - port: 1..65535
     */
    private fun validateEgressAdnlAddress(value: String): String? {
        val trimmed = value.trim()
        if (trimmed.isEmpty()) return "пустая строка"
        val parts = trimmed.split("@", limit = 2)
        if (parts.size != 2) return "ожидается формат <pubkey_hex>@<host:port>"

        val pubkeyHex = parts[0].trim()
        val endpoint = parts[1].trim()
        if (!EGRESS_PUBKEY_HEX_REGEX.matches(pubkeyHex)) {
            return "pubkey_hex должен содержать ровно 64 hex-символа"
        }
        if (endpoint.isEmpty()) return "часть <host:port> пустая"

        val hostPort = parseHostPort(endpoint) ?: return "часть <host:port> не распознана"
        if (hostPort.second !in 1..65535) return "port должен быть в диапазоне 1..65535"
        return null
    }

    private fun parseHostPort(endpoint: String): Pair<String, Int>? {
        return if (endpoint.startsWith("[")) {
            val closeIdx = endpoint.indexOf(']')
            if (closeIdx <= 1 || closeIdx + 1 >= endpoint.length || endpoint[closeIdx + 1] != ':') {
                null
            } else {
                val host = endpoint.substring(1, closeIdx)
                val port = endpoint.substring(closeIdx + 2).toIntOrNull() ?: return null
                host to port
            }
        } else {
            val colonIdx = endpoint.lastIndexOf(':')
            if (colonIdx <= 0 || colonIdx == endpoint.lastIndex) {
                null
            } else {
                val host = endpoint.substring(0, colonIdx)
                val port = endpoint.substring(colonIdx + 1).toIntOrNull() ?: return null
                host to port
            }
        }
    }

    private fun logAdvertiseDecision(egressAdnlAddress: String, advertiseHost: String) {
        val egressHost = parseHostPort(egressAdnlAddress.substringAfter('@', ""))
            ?.first
            ?.trim()
            .orEmpty()
        val egressInet = runCatching { InetAddress.getByName(egressHost) }.getOrNull() as? Inet4Address
        val advertisedInet = runCatching { InetAddress.getByName(advertiseHost.trim()) }.getOrNull() as? Inet4Address
        val mode = when {
            advertiseHost.isBlank() -> "no-advertise"
            advertisedInet == null -> "no-advertise"
            !advertisedInet.isSiteLocalAddress -> "public"
            egressInet != null &&
                egressInet.isSiteLocalAddress &&
                advertisedInet.address[0] == egressInet.address[0] &&
                advertisedInet.address[1] == egressInet.address[1] &&
                advertisedInet.address[2] == egressInet.address[2] -> "LAN"
            else -> "self"
        }
        Log.i(TAG, "advertise mode=$mode advertised address=${if (advertiseHost.isBlank()) "<none>" else advertiseHost}")
    }

    /**
     * Заглушка secure DNS резолвера.
     * Запрещено отправлять DNS как обычный UDP/53 в интернет.
     *
     * TODO: implement secure DNS via TON/ADNL.
     * TODO: Full TCP forwarding requires userspace TCP/IP stack or tun2socks.
     */
    private fun resolveDnsSecurely(queryPacket: ByteArray): ByteArray? {
        return try {
            performDohQueryViaProtectedSocket(queryPacket)
        } catch (_: Throwable) {
            null
        }
    }

    private fun performDohQueryViaProtectedSocket(dnsQuery: ByteArray): ByteArray? {
        val requestHeaders = buildString {
            append("POST /dns-query HTTP/1.1\r\n")
            append("Host: ").append(DOH_HOST).append("\r\n")
            append("Accept: application/dns-message\r\n")
            append("Content-Type: application/dns-message\r\n")
            append("Content-Length: ").append(dnsQuery.size).append("\r\n")
            append("Connection: close\r\n")
            append("\r\n")
        }.toByteArray(Charsets.US_ASCII)

        val rawSocket = Socket()
        if (!protect(rawSocket)) {
            rawSocket.close()
            return null
        }
        rawSocket.connect(InetSocketAddress(DOH_IP, DOH_PORT), DOH_CONNECT_TIMEOUT_MS)
        rawSocket.soTimeout = DOH_READ_TIMEOUT_MS

        val sslSocket = (SSLSocketFactory.getDefault() as SSLSocketFactory)
            .createSocket(rawSocket, DOH_HOST, DOH_PORT, true) as SSLSocket

        sslSocket.use { tls ->
            tls.enabledProtocols = arrayOf("TLSv1.2", "TLSv1.3")
            tls.startHandshake()

            val output = tls.outputStream
            output.write(requestHeaders)
            output.write(dnsQuery)
            output.flush()

            val input = tls.inputStream
            val headerBytes = readHttpHeaders(input) ?: return null
            val headerText = headerBytes.toString(Charsets.US_ASCII)
            if (!headerText.startsWith("HTTP/1.1 200") && !headerText.startsWith("HTTP/1.0 200")) {
                return null
            }
            val contentLength = parseContentLength(headerText) ?: return null
            return readExactBytes(input, contentLength)
        }
    }

    private fun readHttpHeaders(input: InputStream): ByteArray? {
        val buffer = ByteArray(MAX_HTTP_HEADER_BYTES)
        var count = 0
        while (count < buffer.size) {
            val read = input.read()
            if (read == -1) return null
            buffer[count++] = read.toByte()
            if (count >= 4 &&
                buffer[count - 4] == '\r'.code.toByte() &&
                buffer[count - 3] == '\n'.code.toByte() &&
                buffer[count - 2] == '\r'.code.toByte() &&
                buffer[count - 1] == '\n'.code.toByte()
            ) {
                return buffer.copyOf(count)
            }
        }
        return null
    }

    private fun parseContentLength(headerText: String): Int? {
        val lines = headerText.split("\r\n")
        for (line in lines) {
            if (line.startsWith("Content-Length:", ignoreCase = true)) {
                val value = line.substringAfter(':').trim()
                return value.toIntOrNull()
            }
        }
        return null
    }

    private fun readExactBytes(input: InputStream, length: Int): ByteArray? {
        if (length <= 0 || length > DNS_MAX_PACKET_SIZE * 4) return null
        val out = ByteArray(length)
        var offset = 0
        while (offset < length) {
            val read = input.read(out, offset, length - offset)
            if (read <= 0) return null
            offset += read
        }
        return out
    }

    private fun createServiceScope(): CoroutineScope {
        return CoroutineScope(SupervisorJob() + ioDispatcher)
    }

    private fun verifyLocalSocksServerReady(): Boolean {
        return runCatching {
            Socket().use { socket ->
                socket.connect(InetSocketAddress(LOCALHOST, LOCAL_SOCKS_PORT), SOCKS_PROBE_TIMEOUT_MS)
            }
            true
        }.getOrDefault(false)
    }

    private fun protectSocketWithFallback(socket: Socket): Boolean {
        val direct = runCatching { protect(socket) }.getOrDefault(false)
        if (direct) {
            Log.i(VPN_TRAFFIC_TAG, "protected socket success")
            return true
        }

        // На некоторых прошивках protect(Socket) возвращает false.
        // Fallback: пробуем protect(fd), извлекая дескриптор через ParcelFileDescriptor.
        val fallback = runCatching {
            ParcelFileDescriptor.fromSocket(socket).use { pfd ->
                protect(pfd.fd)
            }
        }.getOrDefault(false)
        if (fallback) {
            Log.i(VPN_TRAFFIC_TAG, "protected socket success")
        }
        return fallback
    }

    private fun isLoopTarget(target: InetSocketAddress): Boolean {
        val host = target.hostString ?: return false
        if (host == LOCALHOST || host == "localhost") return true
        if (host.startsWith("10.0.0.")) return true
        return false
    }

    companion object {
        const val ACTION_START_VPN = "com.example.dapf.tongate.action.START_VPN"
        const val ACTION_STOP_VPN = "com.example.dapf.tongate.action.STOP_VPN"
        const val ACTION_VPN_STATS = "com.example.dapf.tongate.action.VPN_STATS"
        const val ACTION_VPN_ERROR = "com.example.dapf.tongate.action.VPN_ERROR"
        const val EXTRA_VPN_ENABLED = "extra_vpn_enabled"
        const val EXTRA_BLOCKED_COUNT = "extra_blocked_count"
        const val EXTRA_ERROR_MESSAGE = "extra_error_message"

        const val VPN_NOTIFICATION_ID = 1101
        const val VPN_CHANNEL_ID = "ton_gate_vpn_channel"
        const val VPN_SESSION_NAME = "Dapf TON Gate"
        const val VPN_MTU = 1300
        const val VPN_IPV4_ADDRESS = "10.0.0.1"
        const val VPN_IPV4_PREFIX = 24
        const val VPN_DNS_LOCALHOST = "127.0.0.1"
        const val DNS_LOCAL_PORT = 53
        const val DNS_MAX_PACKET_SIZE = 512
        const val DNS_SOCKET_TIMEOUT_MS = 1500
        const val TAG = "TonShieldVpnService"
        const val VPN_TRAFFIC_TAG = "VPN_TRAFFIC"
        const val VPN_TUN_WRITE_TAG = "VPN_TUN_WRITE"

        // DNS сервер, объявляемый системе в VPN-профиле.
        // 10.0.0.2 провоцировал петлю (попытки TCP/853 в TUN-адрес),
        // поэтому указываем внешний резолвер: трафик до него идет через tun2socks/SOCKS.
        const val VPN_DNS_SERVER = "1.1.1.1"
        const val TON_INBOUND_IDLE_DELAY_MS = 20L
        const val DNS_FILTER_ENABLED = false
        const val LOCALHOST = "127.0.0.1"
        const val LOCAL_SOCKS_PORT = 1080
        const val DNS_GW_PORT = 5450
        const val SOCKS_CONNECT_TIMEOUT_MS = 10000
        const val SOCKS_PROBE_TIMEOUT_MS = 1500
        const val IPV4_VERSION = 4
        const val IPV6_VERSION = 6
        const val MAX_HTTP_HEADER_BYTES = 8192
        const val DOH_HOST = "cloudflare-dns.com"
        const val DOH_IP = "1.1.1.1"
        const val DOH_PORT = 443
        const val DOH_CONNECT_TIMEOUT_MS = 5000
        const val DOH_READ_TIMEOUT_MS = 7000
        const val TCP_PROTOCOL = 6
        const val HTTP_PORT = 80
        const val HTTPS_PORT = 443
        val EGRESS_PUBKEY_HEX_REGEX = Regex("^[0-9a-fA-F]{64}$")
    }
}
