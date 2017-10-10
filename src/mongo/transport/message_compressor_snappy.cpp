/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include "mongo/base/data_range_cursor.h"
#include "mongo/base/init.h"
#include "mongo/stdx/memory.h"
#include "mongo/transport/message_compressor_registry.h"
#include "mongo/transport/message_compressor_snappy.h"

#include <snappy.h>

namespace mongo {

SnappyMessageCompressor::SnappyMessageCompressor()
    : MessageCompressorBase(MessageCompressor::kSnappy) {}

std::size_t SnappyMessageCompressor::getMaxCompressedSize(size_t inputSize) {
    return snappy::MaxCompressedLength(inputSize);
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
    compressorRegistry.registerImplementation(stdx::make_unique<SnappyMessageCompressor>());
    return Status::OK();
}
}  // namespace mongo
