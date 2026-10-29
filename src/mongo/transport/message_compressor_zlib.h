// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/data_range.h"
#include "mongo/base/status_with.h"
#include "mongo/transport/message_compressor_base.h"
#include "mongo/util/modules.h"

#include <cstddef>

#include <boost/optional/optional.hpp>

namespace mongo {
class [[MONGO_MOD_NEEDS_REPLACEMENT]] ZlibMessageCompressor final : public MessageCompressorBase {
public:
    ZlibMessageCompressor();

    std::size_t getMaxCompressedSize(size_t inputSize) override;

    boost::optional<std::size_t> getMaxDecompressedSize(ConstDataRange input) override;

    StatusWith<std::size_t> compressData(ConstDataRange input, DataRange output) override;

    StatusWith<std::size_t> decompressData(ConstDataRange input, DataRange output) override;
};


}  // namespace mongo
