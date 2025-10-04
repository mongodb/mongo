/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/base/data_range.h"
#include "mongo/base/status_with.h"

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
