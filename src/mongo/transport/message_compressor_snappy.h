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

/**
 * Reads and validates the flags prefix of a Snappy-compressed OP_MSG without decompressing its
 * body. Success does not validate the remaining compressed stream, BSON, or checksum; callers
 * must still fully decompress and parse the message before using its contents. Failure is not
 * authoritative either: noncanonical encodings may be accepted by libsnappy, and a corrupt tail
 * may take precedence over invalid flags. Callers requiring normal decode error semantics must
 * fall back to full decompression and parsing.
 */
[[MONGO_MOD_PUBLIC]] StatusWith<uint32_t> readSnappyOpMsgFlags(const Message& message);

class [[MONGO_MOD_PUBLIC]] SnappyMessageCompressor final : public MessageCompressorBase {
public:
    SnappyMessageCompressor();

    std::size_t getMaxCompressedSize(size_t inputSize) override;

    boost::optional<std::size_t> getMaxDecompressedSize(ConstDataRange input) override;

protected:
    StatusWith<std::size_t> doCompressData(ConstDataRange input, DataRange output) override;

    StatusWith<std::size_t> doDecompressData(ConstDataRange input, DataRange output) override;
};


}  // namespace mongo
