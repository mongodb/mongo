// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/ftdc/decompressor.h"

#include "mongo/base/data_range_cursor.h"
#include "mongo/base/data_type_endian.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/db/ftdc/compressor.h"
#include "mongo/db/ftdc/util.h"
#include "mongo/rpc/object_check.h"
#include "mongo/util/varint.h"

#include <boost/move/utility_core.hpp>

namespace mongo {

StatusWith<std::vector<BSONObj>> FTDCDecompressor::uncompress(ConstDataRange buf) {
    ConstDataRangeCursor compressedDataRange(buf);

    // Read the length of the uncompressed buffer
    auto swUncompressedLength =
        compressedDataRange.readAndAdvanceNoThrow<LittleEndian<std::uint32_t>>();
    if (!swUncompressedLength.isOK()) {
        return {swUncompressedLength.getStatus()};
    }

    // Now uncompress the data
    // Limit size of the buffer we need zlib
    auto uncompressedLength = swUncompressedLength.getValue();

    if (uncompressedLength > kMaxDeltaFileSize) {
        return Status(ErrorCodes::InvalidLength, "Metrics chunk has exceeded the allowable size.");
    }

    auto statusUncompress = _compressor.uncompress(compressedDataRange, uncompressedLength);

    if (!statusUncompress.isOK()) {
        return {statusUncompress.getStatus()};
    }

    ConstDataRangeCursor cdc = statusUncompress.getValue();

    // The document is not part of any checksum so we must validate it is correct
    auto swRef = cdc.readAndAdvanceNoThrow<rpc::ValidatedBSONObj>();
    if (!swRef.isOK()) {
        return {swRef.getStatus()};
    }

    BSONObj ref{swRef.getValue()};

    // Read count of metrics
    auto swMetricsCount = cdc.readAndAdvanceNoThrow<LittleEndian<std::uint32_t>>();
    if (!swMetricsCount.isOK()) {
        return {swMetricsCount.getStatus()};
    }

    std::uint32_t metricsCount = swMetricsCount.getValue();

    // Read count of samples
    auto swSampleCount = cdc.readAndAdvanceNoThrow<LittleEndian<std::uint32_t>>();
    if (!swSampleCount.isOK()) {
        return {swSampleCount.getStatus()};
    }

    std::uint32_t sampleCount = swSampleCount.getValue();

    // Limit memory required to inflate the samples.
    // This is intended to be a canary limit to prevent a pathological file from pre-allocating
    // an insane amount of memory, rather than a limit we expect to see during normal operation.
    auto totalSampleCount =
        static_cast<std::uint64_t>(metricsCount) * static_cast<std::uint64_t>(sampleCount);
    if (totalSampleCount > kMaxTotalSamples) {
        return Status(
            ErrorCodes::InvalidLength,
            fmt::format(
                "Total sample count {} ({} metrics times {} samples) exceeds maximum limit {}",
                totalSampleCount,
                metricsCount,
                sampleCount,
                kMaxTotalSamples));
    }

    // Stores each sample of each metric in a 2D array in sample-major order.
    // The first column IE metrics[0] stores the reference value for each metric.
    // The remainder of the columns store the actual measured values. Given sample N and delta D,
    // sample N + 1 is calculated by summing sample N with D.
    // The deltas are stored in metric-major order (samples of the same metric are adjacent), but
    // the function output expects sample-major order (metrics at a given sample time are adjacent),
    // so unfortunately we need to allocate memory for the entire output in order to transmute the
    // column/row ordering.
    std::vector<std::vector<std::uint64_t>> metrics;
    metrics.reserve(1 + sampleCount);
    metrics.emplace_back();

    // We pass the reference document as both the reference document and current document as we only
    // want the array of metrics.
    (void)FTDCBSONUtil::extractMetricsFromDocument(ref, ref, &metrics[0]);

    if (metrics[0].size() != metricsCount) {
        return {ErrorCodes::BadValue,
                "The metrics in the reference document and metrics count do not match"};
    }

    // Allocate space for the reference document + samples
    std::vector<BSONObj> docs;
    docs.reserve(1 + sampleCount);
    docs.emplace_back(ref.getOwned());

    // We must always return the reference document
    if (sampleCount == 0) {
        return {docs};
    }

    for (std::uint32_t i = 0; i < sampleCount; ++i) {
        metrics.push_back(std::vector<std::uint64_t>(metricsCount));
    }

    // Read the delta values from the decompressed data, carrying forward the previous sample's
    // value
    std::uint64_t zeroesCount = 0;

    for (std::uint32_t i = 0; i < metricsCount; i++) {
        for (std::uint32_t j = 0; j < sampleCount; j++) {
            if (zeroesCount) {
                // This uglyish inner loop is worth over a 10% speedup in cases of long runs of
                // zeroes by allowing the CPU to pipeline multiple stores without dependent loads.
                auto start = j;
                auto end = j;
                auto base = metrics[j][i];

                if (zeroesCount <= sampleCount - j) {
                    end = j + zeroesCount;
                    j += zeroesCount - 1;
                    zeroesCount = 0;
                } else {
                    end = sampleCount;
                    zeroesCount -= sampleCount - j;
                    j = sampleCount - 1;
                }

                for (; start < end; ++start) {
                    metrics[start + 1][i] = base;
                }

                continue;
            }

            auto swDelta = cdc.readAndAdvanceNoThrow<VarInt>();

            if (!swDelta.isOK()) {
                return swDelta.getStatus();
            }

            std::uint64_t v = swDelta.getValue();
            if (v == 0) {
                auto swZero = cdc.readAndAdvanceNoThrow<VarInt>();

                if (!swZero.isOK()) {
                    return swZero.getStatus();
                }

                zeroesCount = swZero.getValue();
            }

            // The previous row of samples (j) for this metric (i) contains the base value for this
            // delta. So, to get the current sample's value (j + 1), add the delta to the
            // base value.
            metrics[j + 1][i] = v + metrics[j][i];
        }
    }

    for (std::uint32_t i = 1; i <= sampleCount; ++i) {
        docs.emplace_back(FTDCBSONUtil::constructDocumentFromMetrics(ref, metrics[i]).getValue());
    }

    return {docs};
}

}  // namespace mongo
