// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/transport/message_compressor_snappy.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/transport/message_compressor_registry.h"

#include <memory>
#include <string>

#include <snappy.h>

#include <boost/move/utility_core.hpp>

namespace mongo {

SnappyMessageCompressor::SnappyMessageCompressor()
    : MessageCompressorBase(MessageCompressor::kSnappy) {}

std::size_t SnappyMessageCompressor::getMaxCompressedSize(size_t inputSize) {
    return snappy::MaxCompressedLength(inputSize);
}

boost::optional<std::size_t> SnappyMessageCompressor::getMaxDecompressedSize(ConstDataRange input) {
    size_t length = 0;
    if (snappy::GetUncompressedLength(input.data(), input.length(), &length)) {
        return length;
    }
    return boost::none;
}

StatusWith<std::size_t> SnappyMessageCompressor::compressData(ConstDataRange input,
                                                              DataRange output) {
    size_t outLength = output.length();
    if (output.length() < getMaxCompressedSize(input.length())) {
        return {ErrorCodes::BadValue, "Output too small for max size of compressed input"};
    }
    snappy::RawCompress(input.data(), input.length(), const_cast<char*>(output.data()), &outLength);

    counterHitCompress(input.length(), outLength);
    return {outLength};
}

StatusWith<std::size_t> SnappyMessageCompressor::decompressData(ConstDataRange input,
                                                                DataRange output) {
    size_t expectedLength = 0;
    if (!snappy::GetUncompressedLength(input.data(), input.length(), &expectedLength) ||
        expectedLength != output.length()) {
        return {ErrorCodes::BadValue, "Compressed message was invalid or corrupted"};
    }

    if (!snappy::RawUncompress(input.data(), input.length(), const_cast<char*>(output.data()))) {
        return Status{ErrorCodes::BadValue, "Compressed message was invalid or corrupted"};
    }

    counterHitDecompress(input.length(), output.length());
    return output.length();
}


MONGO_INITIALIZER_GENERAL(SnappyMessageCompressorInit,
                          ("EndStartupOptionHandling"),
                          ("AllCompressorsRegistered"))
(InitializerContext* context) {
    auto& compressorRegistry = MessageCompressorRegistry::get();
    compressorRegistry.registerImplementation(std::make_unique<SnappyMessageCompressor>());
}
}  // namespace mongo
