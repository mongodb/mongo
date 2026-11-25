// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/data_range.h"
#include "mongo/base/status_with.h"
#include "mongo/rpc/message.h"
#include "mongo/transport/message_compressor_base.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <cstdint>

#include <boost/optional/optional.hpp>

namespace mongo {

inline constexpr size_t kZlibOpMsgPrefixWorkspaceBytes = 64 * 1024;

/**
 * Reads four OP_MSG flags bytes with inflate() and a bounded arena. Success does not validate
 * the tail, BSON or checksum. Failure is not authoritative: callers must fully decompress and
 * parse for normal error semantics, and compare flags on success. The caller must account for
 * kZlibOpMsgPrefixWorkspaceBytes before invoking this helper.
 */
[[MONGO_MOD_PUBLIC]] StatusWith<uint32_t> readZlibOpMsgFlags(const Message& message);

class [[MONGO_MOD_NEEDS_REPLACEMENT]] ZlibMessageCompressor final : public MessageCompressorBase {
public:
    ZlibMessageCompressor();

    std::size_t getMaxCompressedSize(size_t inputSize) override;

    boost::optional<std::size_t> getMaxDecompressedSize(ConstDataRange input) override;

protected:
    StatusWith<std::size_t> doCompressData(ConstDataRange input, DataRange output) override;

    StatusWith<std::size_t> doDecompressData(ConstDataRange input, DataRange output) override;
};


}  // namespace mongo
