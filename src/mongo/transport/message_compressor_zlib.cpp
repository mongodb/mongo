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

    namespace {

    // Minimum valid zlib compressed payload size (header + minimal data + checksum)
    constexpr size_t kMinZlibPayloadSize = 8;

    // Maximum allowed decompression ratio to prevent zip bombs
    // A ratio above 1024:1 is suspicious for typical MongoDB messages
    constexpr size_t kMaxDecompressionRatio = 1024;

    // Validates zlib header bytes for correctness before decompression.
    bool isValidZlibHeader(const Bytef* data, size_t length) {
        if (length < 2) {
            return false;
        }
        
        uint8_t cmf = data[0];
        uint8_t flg = data[1];
        
        // Check compression method is deflate (method 8)
        if ((cmf & 0x0F) != 8) {
            return false;
        }
        
        // Check CMF and FLG checksum (must be multiple of 31)
        if (((static_cast<uint16_t>(cmf) * 256 + flg) % 31) != 0) {
            return false;
        }
        
        return true;
    }
    }  // namespace

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
    // Check 1: Minimum payload size
    if (input.length() < kMinZlibPayloadSize) {
        return Status{ErrorCodes::BadValue, 
            "Compressed payload too small for valid zlib data"};
    }
    
    // Check 2: Validate zlib header format
    if (!isValidZlibHeader(reinterpret_cast<const Bytef*>(input.data()), 
                           input.length())) {
        return Status{ErrorCodes::BadValue, 
            "Invalid zlib header format"};
    }
    
    // Check 3: Decompression ratio sanity check (zip bomb protection)
    if (output.length() > input.length() * kMaxDecompressionRatio) {
        return Status{ErrorCodes::BadValue, 
            "Requested decompression ratio exceeds safety limits"};
    }
    
    uLongf length = output.length();
    int ret = ::uncompress(const_cast<Bytef*>(reinterpret_cast<const Bytef*>(output.data())),
                           &length,
                           reinterpret_cast<const Bytef*>(input.data()),
                           input.length());

    if (ret != Z_OK) {
        return Status{ErrorCodes::BadValue, "Compressed message was invalid or corrupted"};
    }

    // FIX: Use actual decompressed length for metrics
    // Previous code incorrectly used output.length() (buffer size)
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
