// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/transport/message_compressor_base.h"
#include "mongo/util/modules.h"
namespace mongo {

class [[MONGO_MOD_PUBLIC]] NoopMessageCompressor final : public MessageCompressorBase {
public:
    NoopMessageCompressor() : MessageCompressorBase(MessageCompressor::kNoop) {}

    std::size_t getMaxCompressedSize(size_t inputSize) override {
        return inputSize;
    }

    boost::optional<std::size_t> getMaxDecompressedSize(ConstDataRange input) override {
        return boost::none;
    }

    StatusWith<std::size_t> compressData(ConstDataRange input, DataRange output) override try {
        output.write(input);
        counterHitCompress(input.length(), input.length());
        return {input.length()};
    } catch (const DBException& e) {
        return e.toStatus();
    }

    StatusWith<std::size_t> decompressData(ConstDataRange input, DataRange output) override try {
        output.write(input);
        counterHitDecompress(input.length(), input.length());
        return {input.length()};
    } catch (const DBException& e) {
        return e.toStatus();
    }
};
}  // namespace mongo
