// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/ftdc/block_compressor.h"

#include "mongo/base/error_codes.h"
#include "mongo/util/str.h"

#include <zlib.h>

#include <boost/move/utility_core.hpp>

namespace mongo {

StatusWith<ConstDataRange> BlockCompressor::compress(ConstDataRange source) {
    z_stream stream;
    int level = Z_DEFAULT_COMPRESSION;

    stream.next_in = reinterpret_cast<unsigned char*>(const_cast<char*>(source.data()));
    stream.avail_in = source.length();

    // In compress.c in the zlib source, they recommend that the compression buffer be
    // at least 0.1% larger + 12 bytes then the source length.
    // We make the buffer 1% larger then the source length buffer to be on the safe side. If we are
    // too small, deflate returns an error.
    _buffer.resize(source.length() * 1.01 + 12);

    stream.next_out = _buffer.data();
    stream.avail_out = _buffer.size();

    stream.zalloc = nullptr;
    stream.zfree = nullptr;
    stream.opaque = nullptr;

    int err = deflateInit(&stream, level);
    if (err != Z_OK) {
        return {ErrorCodes::ZLibError, str::stream() << "deflateInit failed with " << err};
    }

    err = deflate(&stream, Z_FINISH);
    if (err != Z_STREAM_END) {
        (void)deflateEnd(&stream);

        return {ErrorCodes::ZLibError, str::stream() << "deflate failed with " << err};
    }

    err = deflateEnd(&stream);
    if (err != Z_OK) {
        return {ErrorCodes::ZLibError, str::stream() << "deflateEnd failed with " << err};
    }

    return ConstDataRange(_buffer.data(), stream.total_out);
}

StatusWith<ConstDataRange> BlockCompressor::uncompress(ConstDataRange source,
                                                       size_t uncompressedLength) {
    z_stream stream;

    stream.next_in = reinterpret_cast<unsigned char*>(const_cast<char*>(source.data()));
    stream.avail_in = source.length();

    _buffer.resize(uncompressedLength);

    stream.next_out = _buffer.data();
    stream.avail_out = _buffer.size();

    stream.zalloc = nullptr;
    stream.zfree = nullptr;
    stream.opaque = nullptr;

    int err = inflateInit(&stream);
    if (err != Z_OK) {
        return {ErrorCodes::ZLibError, str::stream() << "inflateInit failed with " << err};
    }

    err = inflate(&stream, Z_FINISH);
    if (err != Z_STREAM_END) {
        (void)inflateEnd(&stream);

        if (err != Z_OK) {
            return {ErrorCodes::ZLibError, str::stream() << "inflate failed with " << err};
        }
    }

    err = inflateEnd(&stream);
    if (err != Z_OK) {
        return {ErrorCodes::ZLibError, str::stream() << "inflateEnd failed with " << err};
    }

    return ConstDataRange(_buffer.data(), stream.total_out);
}

}  // namespace mongo
