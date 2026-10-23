// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <array>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace [[MONGO_MOD_PUBLIC]] mongo {

// The ids are part of the wire protocol and must never change.
#define MONGO_MESSAGE_COMPRESSOR_TABLE(X) \
    X(kNoop, 0, "noop")                   \
    X(kSnappy, 1, "snappy")               \
    X(kZlib, 2, "zlib")                   \
    X(kZstd, 3, "zstd")

enum class MessageCompressor : uint8_t {
#define X(name, id, str) name = id,
    MONGO_MESSAGE_COMPRESSOR_TABLE(X)
#undef X
};

using MessageCompressorId = std::underlying_type_t<MessageCompressor>;

inline constexpr std::array kMessageCompressorNames{
#define X(name, id, str) std::string_view{str},
    MONGO_MESSAGE_COMPRESSOR_TABLE(X)
#undef X
};

#define X(name, id, str) \
    static_assert(kMessageCompressorNames[size_t(MessageCompressor::name)] == str);
MONGO_MESSAGE_COMPRESSOR_TABLE(X)
#undef X

inline std::string_view getMessageCompressorName(MessageCompressor id) {
    const auto idx = size_t(id);
    if (idx >= kMessageCompressorNames.size()) {
        fasserted(13431000);  // Invalid message compressor ID
    }
    return kMessageCompressorNames[idx];
}

/*
 * Which connection role a manager serves: who initiated the connection, not which way a
 * message flows. kUncounted managers attribute traffic to neither role.
 */
enum class MessageCompressionDirection : uint8_t {
    kIngress = 0,
    kEgress = 1,
    kUncounted = 2,
};

// kUncounted has no counters, so it must stay last.
inline constexpr size_t kNumCountedCompressionDirections =
    size_t(MessageCompressionDirection::kUncounted);

}  // namespace mongo
