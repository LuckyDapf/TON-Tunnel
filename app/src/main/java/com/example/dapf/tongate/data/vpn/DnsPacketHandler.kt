package com.example.dapf.tongate.data.vpn

import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.Locale

/**
 * Обработчик DNS-пакетов без внешних библиотек.
 * Работает только с первым вопросом (QDCOUNT=1), что покрывает типичный DNS query от Android приложений.
 */
class DnsPacketHandler(
    blockedDomains: Set<String>
) {

    /**
     * Приводим blacklist к нижнему регистру заранее.
     * Set неизменяемый, поэтому доступ из нескольких потоков безопасен.
     */
    private val blockedDomainSet: Set<String> = blockedDomains.map {
        it.trim().lowercase(Locale.US)
    }.toSet()

    fun handleQuery(packetBytes: ByteArray, packetLength: Int): Result {
        if (packetLength < DNS_HEADER_SIZE) return Result.Invalid

        val parseResult = parseQuestion(packetBytes, packetLength) ?: return Result.Invalid
        val normalizedDomain = parseResult.domainName.lowercase(Locale.US)

        return if (blockedDomainSet.contains(normalizedDomain)) {
            val response = buildNxdomainResponse(
                requestBytes = packetBytes,
                requestLength = packetLength,
                questionEndOffset = parseResult.questionEndOffset
            )
            Result.Blocked(
                domain = normalizedDomain,
                responseBytes = response,
                responseLength = response.size
            )
        } else {
            Result.Allowed(
                domain = normalizedDomain,
                originalPacket = packetBytes.copyOf(packetLength),
                originalLength = packetLength
            )
        }
    }

    /**
     * Парсинг QNAME в формате DNS labels:
     * [6]google[3]com[0]
     */
    private fun parseQuestion(packetBytes: ByteArray, packetLength: Int): ParsedQuestion? {
        val header = ByteBuffer.wrap(packetBytes, 0, DNS_HEADER_SIZE).order(ByteOrder.BIG_ENDIAN)
        header.position(4)
        val qdCount = header.short.toInt() and 0xFFFF
        if (qdCount < 1) return null

        var offset = DNS_HEADER_SIZE
        val labels = mutableListOf<String>()

        while (offset < packetLength) {
            val labelLength = packetBytes[offset].toInt() and 0xFF
            offset += 1

            if (labelLength == 0) {
                break
            }

            // Компрессия имени (pointer) в DNS query бывает редко, но валидируем защитно.
            if ((labelLength and DNS_POINTER_MASK) == DNS_POINTER_MASK) {
                return null
            }

            if (offset + labelLength > packetLength) {
                return null
            }

            val label = String(
                packetBytes,
                offset,
                labelLength,
                Charsets.UTF_8
            )
            labels.add(label)
            offset += labelLength
        }

        // После QNAME обязаны идти QTYPE (2 байта) и QCLASS (2 байта).
        if (offset + DNS_QUESTION_TRAILER_SIZE > packetLength) {
            return null
        }

        // Смещение конца вопроса используется для копирования query-секции в ответ.
        val questionEndOffset = offset + DNS_QUESTION_TRAILER_SIZE

        return ParsedQuestion(
            domainName = labels.joinToString("."),
            questionEndOffset = questionEndOffset
        )
    }

    /**
     * Собирает минимальный валидный DNS response с RCODE=NXDOMAIN.
     * В ответе:
     * - копируем ID из запроса;
     * - ставим флаги: QR=1, RD копируется из запроса, RA=1, RCODE=3;
     * - возвращаем исходный вопрос, но без ответов (ANCOUNT=0).
     */
    private fun buildNxdomainResponse(
        requestBytes: ByteArray,
        requestLength: Int,
        questionEndOffset: Int
    ): ByteArray {
        val requestFlags = ((requestBytes[2].toInt() and 0xFF) shl 8) or (requestBytes[3].toInt() and 0xFF)
        val rdFlag = requestFlags and DNS_FLAG_RD

        val responseFlags = DNS_FLAG_QR or DNS_FLAG_RA or rdFlag or DNS_RCODE_NXDOMAIN
        val responseLength = DNS_HEADER_SIZE + (questionEndOffset - DNS_HEADER_SIZE)

        val response = ByteBuffer.allocate(responseLength).order(ByteOrder.BIG_ENDIAN)

        // Header
        response.put(requestBytes[0]) // ID high
        response.put(requestBytes[1]) // ID low
        response.putShort(responseFlags.toShort())
        response.putShort(1) // QDCOUNT: один вопрос
        response.putShort(0) // ANCOUNT
        response.putShort(0) // NSCOUNT
        response.putShort(0) // ARCOUNT

        // Question section (QNAME + QTYPE + QCLASS) копируем из исходного запроса.
        response.put(requestBytes, DNS_HEADER_SIZE, questionEndOffset - DNS_HEADER_SIZE)

        return response.array()
    }

    data class ParsedQuestion(
        val domainName: String,
        val questionEndOffset: Int
    )

    sealed class Result {
        data class Blocked(
            val domain: String,
            val responseBytes: ByteArray,
            val responseLength: Int
        ) : Result()

        data class Allowed(
            val domain: String,
            val originalPacket: ByteArray,
            val originalLength: Int
        ) : Result()

        data object Invalid : Result()
    }

    private companion object {
        const val DNS_HEADER_SIZE = 12
        const val DNS_QUESTION_TRAILER_SIZE = 4
        const val DNS_POINTER_MASK = 0xC0

        const val DNS_FLAG_QR = 0x8000
        const val DNS_FLAG_RD = 0x0100
        const val DNS_FLAG_RA = 0x0080
        const val DNS_RCODE_NXDOMAIN = 0x0003
    }
}
