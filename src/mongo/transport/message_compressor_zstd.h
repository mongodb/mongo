// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/data_range.h"
#include "mongo/base/status_with.h"
#include "mongo/transport/message_compressor_base.h"
#include "mongo/util/modules.h"

#include <cstddef>

namespace mongo {
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
