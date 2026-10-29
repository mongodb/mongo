// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
#include <boost/none.hpp>

namespace mongo {

ZlibMessageCompressor::ZlibMessageCompressor() : MessageCompressorBase(MessageCompressor::kZlib) {}

std::size_t ZlibMessageCompressor::getMaxCompressedSize(size_t inputSize) {
    return ::compressBound(inputSize);
}

boost::optional<std::size_t> ZlibMessageCompressor::getMaxDecompressedSize(ConstDataRange input) {
    return boost::none;
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
