/**
 * Copyright (C) 2015 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/ftdc/decompressor.h"

#include "mongo/base/data_range_cursor.h"
#include "mongo/base/data_type_validated.h"
#include "mongo/db/ftdc/compressor.h"
#include "mongo/db/ftdc/util.h"
#include "mongo/db/ftdc/varint.h"
#include "mongo/db/jsobj.h"
#include "mongo/rpc/object_check.h"
#include "mongo/util/assert_util.h"

namespace mongo {

StatusWith<std::vector<BSONObj>> FTDCDecompressor::uncompress(ConstDataRange buf) {
    ConstDataRangeCursor compressedDataRange(buf);

    // Read the length of the uncompressed buffer
    auto swUncompressedLength = compressedDataRange.readAndAdvance<LittleEndian<std::uint32_t>>();
    if (!swUncompressedLength.isOK()) {
        return {swUncompressedLength.getStatus()};
    }

    // Now uncompress the data
    // Limit size of the buffer we need zlib
    auto uncompressedLength = swUncompressedLength.getValue();

    if (uncompressedLength > 10000000) {
        return Status(ErrorCodes::InvalidLength, "Metrics chunk has exceeded the allowable size.");
    }

    auto statusUncompress = _compressor.uncompress(compressedDataRange, uncompressedLength);

    if (!statusUncompress.isOK()) {
        return {statusUncompress.getStatus()};
    }

    ConstDataRangeCursor cdc = statusUncompress.getValue();

    // The document is not part of any checksum so we must validate it is correct
    auto swRef = cdc.readAndAdvance<Validated<BSONObj>>();
    if (!swRef.isOK()) {
        return {swRef.getStatus()};
    }

    BSONObj ref = swRef.getValue();

    // Read count of metrics
    auto swMetricsCount = cdc.readAndAdvance<LittleEndian<std::uint32_t>>();
    if (!swMetricsCount.isOK()) {
        return {swMetricsCount.getStatus()};
    }

    std::uint32_t metricsCount = swMetricsCount.getValue();

    // Read count of samples
    auto swSampleCount = cdc.readAndAdvance<LittleEndian<std::uint32_t>>();
    if (!swSampleCount.isOK()) {
        return {swSampleCount.getStatus()};
    }

    std::uint32_t sampleCount = swSampleCount.getValue();

    // Limit size of the buffer we need for metrics and samples
    if (metricsCount * sampleCount > 1000000) {
        return Status(ErrorCodes::InvalidLength,
                      "Metrics Count and Sample Count have exceeded the allowable range.");
    }

    std::vector<std::uint64_t> metrics;

    metrics.reserve(metricsCount);

    // We pass the reference document as both the reference document and current document as we only
    // want the array of metrics.
    (void)FTDCBSONUtil::extractMetricsFromDocument(ref, ref, &metrics);

    if (metrics.size() != metricsCount) {
        return {ErrorCodes::BadValue,
                "The metrics in the reference document and metrics count do not match"};
    }

    std::vector<BSONObj> docs;

    // Allocate space for the reference document + samples
    docs.reserve(1 + sampleCount);

    docs.emplace_back(ref.getOwned());

    // We must always return the reference document
    if (sampleCount == 0) {
        return {docs};
    }

    // Read the samples
    std::vector<std::uint64_t> deltas(metricsCount * sampleCount);

    // decompress the deltas
    std::uint64_t zeroesCount = 0;

    auto cdrc = ConstDataRangeCursor(cdc);

    for (std::uint32_t i = 0; i < metricsCount; i++) {
        for (std::uint32_t j = 0; j < sampleCount; j++) {
            if (zeroesCount) {
                deltas[FTDCCompressor::getArrayOffset(sampleCount, j, i)] = 0;
                zeroesCount--;
                continue;
            }

            auto swDelta = cdrc.readAndAdvance<FTDCVarInt>();

            if (!swDelta.isOK()) {
                return swDelta.getStatus();
            }

            if (swDelta.getValue() == 0) {
                auto swZero = cdrc.readAndAdvance<FTDCVarInt>();

                if (!swZero.isOK()) {
                    return swDelta.getStatus();
                }

                zeroesCount = swZero.getValue();
            }

            deltas[FTDCCompressor::getArrayOffset(sampleCount, j, i)] = swDelta.getValue();
        }
    }

    // Inflate the deltas
    for (std::uint32_t i = 0; i < metricsCount; i++) {
        deltas[FTDCCompressor::getArrayOffset(sampleCount, 0, i)] += metrics[i];
    }

    for (std::uint32_t i = 0; i < metricsCount; i++) {
        for (std::uint32_t j = 1; j < sampleCount; j++) {
            deltas[FTDCCompressor::getArrayOffset(sampleCount, j, i)] +=
                deltas[FTDCCompressor::getArrayOffset(sampleCount, j - 1, i)];
        }
    }

    for (std::uint32_t i = 0; i < sampleCount; ++i) {
        for (std::uint32_t j = 0; j < metricsCount; ++j) {
            metrics[j] = deltas[j * sampleCount + i];
        }

        docs.emplace_back(FTDCBSONUtil::constructDocumentFromMetrics(ref, metrics).getValue());
    }

    return {docs};
}

}  // namespace mongo
