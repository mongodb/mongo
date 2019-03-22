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

#include "mongo/platform/basic.h"

#include "mongo/db/ftdc/block_compressor.h"

#include <zlib.h>

#include "mongo/util/mongoutils/str.h"

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

    return ConstDataRange(reinterpret_cast<char*>(_buffer.data()), stream.total_out);
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

    return ConstDataRange(reinterpret_cast<char*>(_buffer.data()), stream.total_out);
}

}  // namespace mongo
