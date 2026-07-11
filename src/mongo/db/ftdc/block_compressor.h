// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/data_range.h"
#include "mongo/base/status_with.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mongo {

/**
 * Compesses and uncompresses a block of buffer using zlib.
 */
class BlockCompressor {
    BlockCompressor(const BlockCompressor&) = delete;
    BlockCompressor& operator=(const BlockCompressor&) = delete;

public:
    BlockCompressor() = default;

    /**
     * Compress a buffer of data.
     *
     * Returns a pointer to a buffer that BlockCompressor owns.
     * The returned buffer is valid until the next call to compress or uncompress.
     */
    StatusWith<ConstDataRange> compress(ConstDataRange source);

    /**
     * Uncompress a buffer of data.
     *
     * maxUncompressedLength is the upper bound on the size of the uncompressed data
     * so that an internal buffer can be allocated to fit it.
     *
     * Returns a pointer to a buffer that BlockCompressor owns.
     * The returned buffer is valid until the next call to compress or uncompress.
     */
    StatusWith<ConstDataRange> uncompress(ConstDataRange source, size_t maxUncompressedLength);

private:
    std::vector<std::uint8_t> _buffer;
};

}  // namespace mongo
