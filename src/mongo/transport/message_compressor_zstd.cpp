// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
#include <boost/optional/optional.hpp>

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

boost::optional<std::size_t> ZstdMessageCompressor::getMaxDecompressedSize(ConstDataRange input) {
    auto maxDecompressedSize = ZSTD_getFrameContentSize(input.data(), input.length());
    if (maxDecompressedSize == ZSTD_CONTENTSIZE_UNKNOWN ||
        maxDecompressedSize == ZSTD_CONTENTSIZE_ERROR) {
        return boost::none;
    }
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
