#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

enum class FrameType : uint8_t {
    Unknown = 0,
    Open = 1,
    Data = 2,
    Close = 3,
    Error = 4,
};

struct InboundFrame {
    FrameType type{FrameType::Unknown};
    uint32_t streamId{0};
    std::vector<uint8_t> payload;
    std::string errorMessage;
    std::vector<uint8_t> rawFrame;
};

std::vector<uint8_t> encodeOpenFrame(uint32_t streamId, const std::string& host, uint16_t port, const std::string& token);
std::vector<uint8_t> encodeOpenAckFrame(uint32_t streamId);
std::vector<uint8_t> encodeDataFrame(uint32_t streamId, const std::vector<uint8_t>& payload);
std::vector<uint8_t> encodeCloseFrame(uint32_t streamId);
std::vector<uint8_t> encodeErrorFrame(uint32_t streamId, const std::string& message);
std::vector<InboundFrame> decodeInboundFrames(const std::vector<uint8_t>& bytes);

