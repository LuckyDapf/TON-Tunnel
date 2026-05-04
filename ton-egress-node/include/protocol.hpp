#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

enum class MessageType : uint8_t {
    Open = 1,
    Data = 2,
    Close = 3,
    Error = 4
};

struct OpenMessage {
    uint32_t stream_id = 0;
    std::string host;
    uint16_t port = 0;
    std::string token;
};

struct DataMessage {
    uint32_t stream_id = 0;
    std::vector<uint8_t> payload;
};

struct CloseMessage {
    uint32_t stream_id = 0;
};

struct ErrorMessage {
    uint32_t stream_id = 0;
    std::string message;
};

struct DecodedMessage {
    MessageType type;
    std::optional<OpenMessage> open;
    std::optional<DataMessage> data;
    std::optional<CloseMessage> close;
    std::optional<ErrorMessage> error;
};

std::optional<DecodedMessage> decodeMessage(const std::vector<uint8_t>& input);
std::vector<uint8_t> encodeOpen(const CloseMessage& msg);
std::vector<uint8_t> encodeData(const DataMessage& msg);
std::vector<uint8_t> encodeError(const ErrorMessage& msg);
std::vector<uint8_t> encodeClose(const CloseMessage& msg);
