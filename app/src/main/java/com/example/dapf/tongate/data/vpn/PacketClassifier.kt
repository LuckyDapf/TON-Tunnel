package com.example.dapf.tongate.data.vpn

/**
 * Классификатор VPN-пакетов из TUN.
 * Нужен как защитный слой перед любыми вызовами tonlib API:
 * raw IP (TCP/UDP/DNS/HTTP/HTTPS) не является TON BOC и не должен уходить в tonlib.
 */
object PacketClassifier {

    data class Info(
        val ipv4: Boolean = false,
        val protocol: Int = -1,
        val dstPort: Int = -1,
        val isDns: Boolean = false,
        val isHttp: Boolean = false,
        val isHttps: Boolean = false
    )

    fun parse(packet: ByteArray, length: Int): Info {
        if (length < IPV4_MIN_HEADER_BYTES) return Info()

        val version = (packet[0].toInt() ushr 4) and VERSION_MASK
        if (version != IPV4_VERSION) return Info()

        val ihlBytes = (packet[0].toInt() and IHL_MASK) * WORD_SIZE_BYTES
        if (ihlBytes < IPV4_MIN_HEADER_BYTES || length < ihlBytes + MIN_L4_PORT_WINDOW_BYTES) {
            return Info(ipv4 = true)
        }

        val protocol = packet[IP_PROTOCOL_OFFSET].toInt() and BYTE_MASK
        var dstPort = -1
        if (protocol == TCP_PROTOCOL || protocol == UDP_PROTOCOL) {
            val dstPortOffset = ihlBytes + DST_PORT_OFFSET_IN_L4_HEADER
            if (length >= dstPortOffset + 2) {
                dstPort = ((packet[dstPortOffset].toInt() and BYTE_MASK) shl 8) or
                    (packet[dstPortOffset + 1].toInt() and BYTE_MASK)
            }
        }

        val isDns = dstPort == DNS_PORT
        val isHttp = protocol == TCP_PROTOCOL && dstPort == HTTP_PORT
        val isHttps = protocol == TCP_PROTOCOL && dstPort == HTTPS_PORT

        return Info(
            ipv4 = true,
            protocol = protocol,
            dstPort = dstPort,
            isDns = isDns,
            isHttp = isHttp,
            isHttps = isHttps
        )
    }

    /**
     * Безопасный режим: любой raw VPN/TUN пакет считаем non-BOC и запрещаем отправку в tonlib.
     */
    fun shouldSendToTonlibFromTunPacket(): Boolean = false

    private const val IPV4_VERSION = 4
    private const val IPV4_MIN_HEADER_BYTES = 20
    private const val MIN_L4_PORT_WINDOW_BYTES = 4
    private const val WORD_SIZE_BYTES = 4
    private const val VERSION_MASK = 0x0F
    private const val IHL_MASK = 0x0F
    private const val IP_PROTOCOL_OFFSET = 9
    private const val DST_PORT_OFFSET_IN_L4_HEADER = 2
    private const val BYTE_MASK = 0xFF

    private const val TCP_PROTOCOL = 6
    private const val UDP_PROTOCOL = 17
    private const val DNS_PORT = 53
    private const val HTTP_PORT = 80
    private const val HTTPS_PORT = 443
}
