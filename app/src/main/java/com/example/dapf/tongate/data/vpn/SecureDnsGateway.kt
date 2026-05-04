package com.example.dapf.tongate.data.vpn

import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.InetSocketAddress
import java.util.concurrent.atomic.AtomicBoolean

class SecureDnsGateway(
    private val bindAddress: String,
    private val bindPort: Int,
    private val resolver: (ByteArray) -> ByteArray?
) {
    private val running = AtomicBoolean(false)
    private var worker: Thread? = null
    private var socket: DatagramSocket? = null

    fun start() {
        if (!running.compareAndSet(false, true)) return
        val localAddress = InetAddress.getByName(bindAddress)
        val datagramSocket = DatagramSocket(null).apply {
            reuseAddress = true
            soTimeout = 1500
            bind(InetSocketAddress(localAddress, bindPort))
        }
        socket = datagramSocket

        worker = Thread {
            val buf = ByteArray(2048)
            val packet = DatagramPacket(buf, buf.size)
            while (running.get()) {
                try {
                    packet.length = buf.size
                    datagramSocket.receive(packet)
                    val query = packet.data.copyOf(packet.length)
                    val response = resolver(query)
                    if (response != null && response.isNotEmpty()) {
                        datagramSocket.send(
                            DatagramPacket(response, response.size, packet.address, packet.port)
                        )
                    }
                } catch (_: Throwable) {
                    // ignore timeouts/shutdown races
                }
            }
        }.apply {
            name = "SecureDnsGateway"
            isDaemon = true
            start()
        }
    }

    fun stop() {
        running.set(false)
        runCatching { socket?.close() }
        socket = null
        runCatching { worker?.interrupt() }
        worker = null
    }
}
