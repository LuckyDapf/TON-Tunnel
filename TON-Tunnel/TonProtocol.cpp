#include "TonProtocol.hpp"

namespace {

uint16_t readU16(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

uint32_t readU32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
        (static_cast<uint32_t>(p[1]) << 16) |
        (static_cast<uint32_t>(p[2]) << 8) |
        static_cast<uint32_t>(p[3]);
}

void writeU16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(v & 0xFF));
}

void writeU32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(v & 0xFF));
}

}

std::vector<uint8_t> encodeOpenFrame(uint32_t streamId, const std::string& host, uint16_t port, const std::string& token) {
    std::vector<uint8_t> out;
    out.reserve(1 + 4 + 2 + host.size() + 2 + 2 + token.size());
    out.push_back(static_cast<uint8_t>(FrameType::Open));
    writeU32(out, streamId);
    writeU16(out, static_cast<uint16_t>(host.size()));
    out.insert(out.end(), host.begin(), host.end());
    writeU16(out, port);
    writeU16(out, static_cast<uint16_t>(token.size()));
    out.insert(out.end(), token.begin(), token.end());
    return out;
}

std::vector<uint8_t> encodeOpenAckFrame(uint32_t streamId) {
    std::vector<uint8_t> out;
    out.reserve(5);
    out.push_back(static_cast<uint8_t>(FrameType::Open));
    writeU32(out, streamId);
    return out;
}

std::vector<uint8_t> encodeDataFrame(uint32_t streamId, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> out;
    out.reserve(1 + 4 + 4 + payload.size());
    out.push_back(static_cast<uint8_t>(FrameType::Data));
    writeU32(out, streamId);
    writeU32(out, static_cast<uint32_t>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::vector<uint8_t> encodeCloseFrame(uint32_t streamId) {
    std::vector<uint8_t> out;
    out.reserve(5);
    out.push_back(static_cast<uint8_t>(FrameType::Close));
    writeU32(out, streamId);
    return out;
}

std::vector<uint8_t> encodeErrorFrame(uint32_t streamId, const std::string& message) {
    std::vector<uint8_t> out;
    out.reserve(1 + 4 + 2 + message.size());
    out.push_back(static_cast<uint8_t>(FrameType::Error));
    writeU32(out, streamId);
    writeU16(out, static_cast<uint16_t>(message.size()));
    out.insert(out.end(), message.begin(), message.end());
    return out;
}

std::vector<InboundFrame> decodeInboundFrames(const std::vector<uint8_t>& bytes) {
    std::vector<InboundFrame> out;
    size_t cursor = 0;
    while (cursor + 5 <= bytes.size()) {
        const auto frameStart = cursor;
        const auto type = static_cast<FrameType>(bytes[cursor++]);
        const uint32_t streamId = readU32(bytes.data() + cursor);
        cursor += 4;
        InboundFrame f{};
        f.type = type;
        f.streamId = streamId;
        switch (type) {
        case FrameType::Open: {
            // OPEN request frame:
            // [type=1][streamId][hostLen:u16][host bytes][port:u16][tokenLen:u16][token bytes]
            // OPEN_ACK frame:
            // [type=1][streamId]
            // We support both encodings to keep parser compatible with request and ack packets.
            if (cursor + 2 <= bytes.size()) {
                const size_t save = cursor;
                const uint16_t hostLen = readU16(bytes.data() + cursor);
                cursor += 2;
                if (cursor + hostLen + 2 + 2 <= bytes.size()) {
                    cursor += hostLen; // host bytes
                    cursor += 2;       // port
                    const uint16_t tokenLen = readU16(bytes.data() + cursor);
                    cursor += 2;
                    if (cursor + tokenLen <= bytes.size()) {
                        cursor += tokenLen; // token bytes
                        break;
                    }
                }
                // Not a full OPEN request body; treat as OPEN_ACK and rewind.
                cursor = save;
            }
            break;
        }
        case FrameType::Close:
            break;
        case FrameType::Data: {
            if (cursor + 4 > bytes.size()) return {};
            const uint32_t payloadLen = readU32(bytes.data() + cursor);
            cursor += 4;
            if (cursor + payloadLen > bytes.size()) return {};
            f.payload.assign(bytes.begin() + static_cast<long long>(cursor), bytes.begin() + static_cast<long long>(cursor + payloadLen));
            cursor += payloadLen;
            break;
        }
        case FrameType::Error: {
            if (cursor + 2 > bytes.size()) return {};
            const uint16_t msgLen = readU16(bytes.data() + cursor);
            cursor += 2;
            if (cursor + msgLen > bytes.size()) return {};
            f.errorMessage.assign(reinterpret_cast<const char*>(bytes.data() + cursor), msgLen);
            cursor += msgLen;
            break;
        }
        default:
            return {};
        }
        f.rawFrame.assign(bytes.begin() + static_cast<long long>(frameStart), bytes.begin() + static_cast<long long>(cursor));
        out.push_back(std::move(f));
    }
    return out;
}

