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

namespace mongo {

inline constexpr size_t kZstdOpMsgPrefixWorkspaceBytes = 10 * 1024 * 1024;

/**
 * Reads four OP_MSG flags bytes using a bounded streaming workspace. Success does not validate
 * the tail, BSON or checksum. Failure (including a workspace/window limit) is not authoritative:
 * callers must fully decompress and parse for normal error semantics, and compare flags on success.
 * The caller must account for kZstdOpMsgPrefixWorkspaceBytes before invoking this helper.
 */
[[MONGO_MOD_PUBLIC]] StatusWith<uint32_t> readZstdOpMsgFlags(const Message& message);

class [[MONGO_MOD_NEEDS_REPLACEMENT]] ZstdMessageCompressor final : public MessageCompressorBase {
public:
    ZstdMessageCompressor();

    std::size_t getMaxCompressedSize(size_t inputSize) override;

    boost::optional<std::size_t> getMaxDecompressedSize(ConstDataRange input) override;

protected:
    StatusWith<std::size_t> doCompressData(ConstDataRange input, DataRange output) override;

    StatusWith<std::size_t> doDecompressData(ConstDataRange input, DataRange output) override;
};


}  // namespace mongo
