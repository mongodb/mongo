// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/data_range.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/ftdc/block_compressor.h"
#include "mongo/util/modules.h"

#include <vector>

namespace mongo {

/**
 * Inflates a compressed chunk of metrics into a list of BSON documents
 */
class FTDCDecompressor {
    FTDCDecompressor(const FTDCDecompressor&) = delete;
    FTDCDecompressor& operator=(const FTDCDecompressor&) = delete;

public:
    FTDCDecompressor() = default;

    /**
     * Inflates a compressed chunk of metrics into a vector of owned BSON documents.
     *
     * Will fail if the chunk is corrupt or too short.
     *
     * Returns N samples where N = sample count + 1. The 1 is the reference document.
     */
    StatusWith<std::vector<BSONObj>> uncompress(ConstDataRange buf);

private:
    BlockCompressor _compressor;
};

}  // namespace mongo
