#include "protocol.hpp"

#include <cstring>

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
} // namespace

std::optional<DecodedMessage> decodeMessage(const std::vector<uint8_t>& input) {
    if (input.size() < 5) return std::nullopt;
    const auto type = static_cast<MessageType>(input[0]);
    const uint32_t streamId = readU32(input.data() + 1);
    size_t cursor = 5;

    DecodedMessage out{.type = type};
    if (type == MessageType::Open) {
        if (input.size() < cursor + 2) return std::nullopt;
        const uint16_t hostLen = readU16(input.data() + cursor);
        cursor += 2;
        if (input.size() < cursor + hostLen + 2 + 2) return std::nullopt;
        std::string host(reinterpret_cast<const char*>(input.data() + cursor), hostLen);
        cursor += hostLen;
        const uint16_t port = readU16(input.data() + cursor);
        cursor += 2;
        const uint16_t tokenLen = readU16(input.data() + cursor);
        cursor += 2;
        if (input.size() < cursor + tokenLen) return std::nullopt;
        std::string token(reinterpret_cast<const char*>(input.data() + cursor), tokenLen);
        out.open = OpenMessage{streamId, std::move(host), port, std::move(token)};
        return out;
    }
    if (type == MessageType::Data) {
        if (input.size() < cursor + 4) return std::nullopt;
        const uint32_t payloadLen = readU32(input.data() + cursor);
        cursor += 4;
        if (input.size() < cursor + payloadLen) return std::nullopt;
        DataMessage d;
        d.stream_id = streamId;
        d.payload.assign(input.begin() + static_cast<long long>(cursor),
                         input.begin() + static_cast<long long>(cursor + payloadLen));
        out.data = std::move(d);
        return out;
    }
    if (type == MessageType::Close) {
        out.close = CloseMessage{streamId};
        return out;
    }
    if (type == MessageType::Error) {
        if (input.size() < cursor + 2) return std::nullopt;
        const uint16_t msgLen = readU16(input.data() + cursor);
        cursor += 2;
        if (input.size() < cursor + msgLen) return std::nullopt;
        out.error = ErrorMessage{
            streamId,
            std::string(reinterpret_cast<const char*>(input.data() + cursor), msgLen)};
        return out;
    }
    return std::nullopt;
}

std::vector<uint8_t> encodeData(const DataMessage& msg) {
    std::vector<uint8_t> out;
    out.reserve(1 + 4 + 4 + msg.payload.size());
    out.push_back(static_cast<uint8_t>(MessageType::Data));
    writeU32(out, msg.stream_id);
    writeU32(out, static_cast<uint32_t>(msg.payload.size()));
    out.insert(out.end(), msg.payload.begin(), msg.payload.end());
    return out;
}

std::vector<uint8_t> encodeOpen(const CloseMessage& msg) {
    std::vector<uint8_t> out;
    out.push_back(static_cast<uint8_t>(MessageType::Open));
    writeU32(out, msg.stream_id);
    return out;
}

std::vector<uint8_t> encodeError(const ErrorMessage& msg) {
    std::vector<uint8_t> out;
    out.push_back(static_cast<uint8_t>(MessageType::Error));
    writeU32(out, msg.stream_id);
    writeU16(out, static_cast<uint16_t>(msg.message.size()));
    out.insert(out.end(), msg.message.begin(), msg.message.end());
    return out;
}

std::vector<uint8_t> encodeClose(const CloseMessage& msg) {
    std::vector<uint8_t> out;
    out.push_back(static_cast<uint8_t>(MessageType::Close));
    writeU32(out, msg.stream_id);
    return out;
}
