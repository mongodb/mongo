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

#include "mongo/transport/message_compressor_zlib.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/transport/message_compressor_registry.h"

#include <memory>
#include <string>

#include <zconf.h>
#include <zlib.h>

#include <boost/move/utility_core.hpp>

namespace mongo {

ZlibMessageCompressor::ZlibMessageCompressor() : MessageCompressorBase(MessageCompressor::kZlib) {}

std::size_t ZlibMessageCompressor::getMaxCompressedSize(size_t inputSize) {
    return ::compressBound(inputSize);
}

StatusWith<std::size_t> ZlibMessageCompressor::compressData(ConstDataRange input,
                                                            DataRange output) {
    size_t outLength = output.length();
    int ret = ::compress2(const_cast<Bytef*>(reinterpret_cast<const Bytef*>(output.data())),
                          reinterpret_cast<uLongf*>(&outLength),
                          reinterpret_cast<const Bytef*>(input.data()),
                          input.length(),
                          Z_DEFAULT_COMPRESSION);

    if (ret != Z_OK) {
        return Status{ErrorCodes::BadValue, "Could not compress input"};
    }
    counterHitCompress(input.length(), outLength);
    return {outLength};
}

StatusWith<std::size_t> ZlibMessageCompressor::decompressData(ConstDataRange input,
                                                              DataRange output) {
    if (input.length() < 8)  // header + minimal data + checksum
        return Status{ErrorCodes::BadValue, "Compressed data too small"};
    // Per RFC1950, validate first two bytes: CMF, FLG.
    uint8_t cmf = input.data()[0];
    uint8_t flg = input.data()[1];
    // `CM` (compression method) is `CMF[0:3]`.
    if ((cmf & 0xf) != Z_DEFLATED)
        return Status{ErrorCodes::BadValue, "zlib compression method != Z_DEFLATED"};
    // `FCHECK` is `FLG[0:4]`:
    //     The FCHECK value must be such that CMF and FLG, when viewed as
    //     a 16-bit unsigned integer stored in MSB order (CMF*256 + FLG),
    //     is a multiple of 31.
    if ((cmf * 256 + flg) % 31)
        return Status{ErrorCodes::BadValue, "zlib header checksum fail"};

    uLongf length = output.length();
    int ret = ::uncompress(const_cast<Bytef*>(reinterpret_cast<const Bytef*>(output.data())),
                           &length,
                           reinterpret_cast<const Bytef*>(input.data()),
                           input.length());

    if (ret != Z_OK) {
        return Status{ErrorCodes::BadValue, "Compressed message was invalid or corrupted"};
    }

    counterHitDecompress(input.length(), length);
    return {length};
}


MONGO_INITIALIZER_GENERAL(ZlibMessageCompressorInit,
                          ("EndStartupOptionHandling"),
                          ("AllCompressorsRegistered"))
(InitializerContext* context) {
    auto& compressorRegistry = MessageCompressorRegistry::get();
    compressorRegistry.registerImplementation(std::make_unique<ZlibMessageCompressor>());
}
}  // namespace mongo
