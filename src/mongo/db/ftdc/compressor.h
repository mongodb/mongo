// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/data_range.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/ftdc/block_compressor.h"
#include "mongo/db/ftdc/config.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <cstddef>
#include <cstdint>
#include <tuple>
#include <vector>

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * FTDCCompressor is responsible for taking a set of BSON documents containing metrics, and
 * compressing them into a highly compressed buffer. Metrics are defined as BSON number or number
 * like type (like dates, and timestamps).
 *
 * Compression Method
 * 1. For each document after the first, it computes the delta between it and the preceding document
 *    for the number fields
 * 2. It stores the deltas into an array of std::int64_t.
 * 3. It compressed each std::int64_t using VarInt integer compression. See varint.h.
 * 4. Encodes zeros in Run Length Encoded pairs of <Count, Zero>
 * 5. ZLIB compresses the final processed array
 *
 * NOTE: This compression ignores non-number data, and assumes the non-number data is constant
 * across all documents in the series of documents.
 */
class FTDCCompressor {
    FTDCCompressor(const FTDCCompressor&) = delete;
    FTDCCompressor& operator=(const FTDCCompressor&) = delete;

public:
    /**
     * The FTDCCompressor returns one of these values when a sample is added to indicate whether the
     * caller should flush the buffer to disk or not.
     */
    enum class CompressorState {
        /**
         * Needs to flush because the schemas has changed. Caller needs to flush.
         */
        kSchemaChanged,

        /**
         * Quota on the number of samples in a metric chunk has been reached. Caller needs to flush.
         */
        kCompressorFull,
    };

    explicit FTDCCompressor(const FTDCConfig* config) : _config(config) {}

    /**
     * Add a bson document containing metrics into the compressor.
     *
     * Returns flag indicating whether the caller should flush the compressor buffer to disk.
     *  1. kCompressorFull if the compressor is considered full.
     *  2. kSchemaChanged if there was a schema change, and buffer should be flushed.
     *  3. kHasSpace if it has room for more metrics in the current buffer.
     *
     * date is the date at which the sample as started to be captured. It will be saved in the
     * compressor if this sample is used as the reference document.N
     */
    StatusWith<boost::optional<std::tuple<ConstDataRange, CompressorState, Date_t>>> addSample(
        const BSONObj& sample, Date_t date);

    /**
     * Returns the number of enqueued samples.
     *
     * The a buffer will decompress to (1 + getCountCount). The extra 1 comes
     * from the reference document.
     */
    std::size_t getSampleCount() const {
        // TODO: This method should probably be renamed, since it currently
        // returns the number of deltas, which does not include the sample
        // implicitly contained in the reference document.
        return _deltaCount;
    }

    /**
     * Has a document been added?
     *
     * If addSample has been called, then we have at least the reference document, but not
     * necessarily any additional metric samples. When the buffer is filled to capacity,
     * the reference document is reset.
     */
    bool hasDataToFlush() const {
        return !_referenceDoc.isEmpty();
    }

    /**
     * Gets buffer of compressed data contained in the FTDCCompressor.
     *
     * The returned buffer is valid until next call to addSample() or getCompressedSamples() with
     * CompressBuffer::kGenerateNewCompressedBuffer.
     */
    StatusWith<std::tuple<ConstDataRange, Date_t>> getCompressedSamples();

    /**
     * Reset the state of the compressor.
     *
     * Callers can use this to reset the compressor to a clean state instead of recreating it.
     */
    void reset();

    /**
     * Compute the offset into an array for given (sample, metric) pair
     */
    static size_t getArrayOffset(std::uint32_t sampleCount,
                                 std::uint32_t sample,
                                 std::uint32_t metric) {
        return metric * sampleCount + sample;
    }

private:
    /**
     * Reset the state
     */
    void _reset(const BSONObj& referenceDoc, Date_t date);

private:
    // Block Compressor
    BlockCompressor _compressor;

    // Config
    const FTDCConfig* const _config;

    // Reference schema document
    BSONObj _referenceDoc;

    // Time at which reference schema document was collected.
    // Passed in via addSample and returned with each chunk.
    Date_t _referenceDocDate;

    // Number of Metrics for the reference document
    std::uint32_t _metricsCount{0};

    // Number of deltas recorded
    std::uint32_t _deltaCount{0};

    // Max deltas for the current chunk
    std::size_t _maxDeltas{0};

    // Array of deltas - M x S
    // _deltas[Metrics][Samples]
    std::vector<std::uint64_t> _deltas;

    // Buffer for metric chunk compressed = uncompressed length + compressed data
    BufBuilder _compressedChunkBuffer;

    // Buffer for uncompressed metric chunk
    BufBuilder _uncompressedChunkBuffer;

    // Buffer to hold metrics
    std::vector<std::uint64_t> _metrics;
    std::vector<std::uint64_t> _prevmetrics;
};

}  // namespace mongo
