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

#include "mongo/transport/message_compressor_zstd.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/transport/message_compressor_registry.h"
#include "mongo/util/str.h"

#include <memory>
#include <string>

#include <zstd.h>

#include <boost/move/utility_core.hpp>

namespace mongo {

ZstdMessageCompressor::ZstdMessageCompressor() : MessageCompressorBase(MessageCompressor::kZstd) {}

std::size_t ZstdMessageCompressor::getMaxCompressedSize(size_t inputSize) {
    return ZSTD_compressBound(inputSize);
}

StatusWith<std::size_t> ZstdMessageCompressor::compressData(ConstDataRange input,
                                                            DataRange output) {
    size_t ret = ZSTD_compress(const_cast<char*>(output.data()),
                               output.length(),
                               input.data(),
                               input.length(),
                               ZSTD_CLEVEL_DEFAULT);

    if (ZSTD_isError(ret)) {
        return Status{ErrorCodes::BadValue,
                      str::stream() << "Could not compress input: " << ZSTD_getErrorName(ret)};
    }
    counterHitCompress(input.length(), ret);
    return {ret};
}

StatusWith<std::size_t> ZstdMessageCompressor::decompressData(ConstDataRange input,
                                                              DataRange output) {
    size_t ret = ZSTD_decompress(
        const_cast<char*>(output.data()), output.length(), input.data(), input.length());

    if (ZSTD_isError(ret)) {
        return Status{ErrorCodes::BadValue,
                      str::stream() << "Could not decompress message: " << ZSTD_getErrorName(ret)};
    }

    counterHitDecompress(input.length(), ret);
    return {ret};
}

std::size_t ZstdMessageCompressor::getMaxDecompressedSize(const void* src, size_t srcSize) {
    auto maxDecompressedSize = ZSTD_getFrameContentSize(src, srcSize);
    return static_cast<size_t>(maxDecompressedSize);
}


MONGO_INITIALIZER_GENERAL(ZstdMessageCompressorInit,
                          ("EndStartupOptionHandling"),
                          ("AllCompressorsRegistered"))
(InitializerContext* context) {
    auto& compressorRegistry = MessageCompressorRegistry::get();
    compressorRegistry.registerImplementation(std::make_unique<ZstdMessageCompressor>());
}
}  // namespace mongo
