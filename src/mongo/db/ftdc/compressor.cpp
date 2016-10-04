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

#include "mongo/db/ftdc/compressor.h"

#include "mongo/base/data_builder.h"
#include "mongo/db/ftdc/config.h"
#include "mongo/db/ftdc/util.h"
#include "mongo/db/ftdc/varint.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/service_context.h"
#include "mongo/util/assert_util.h"

namespace mongo {

using std::swap;

StatusWith<boost::optional<std::tuple<ConstDataRange, FTDCCompressor::CompressorState, Date_t>>>
FTDCCompressor::addSample(const BSONObj& sample, Date_t date) {
    if (_referenceDoc.isEmpty()) {
        FTDCBSONUtil::extractMetricsFromDocument(sample, sample, &_metrics);
        _reset(sample, date);
        return {boost::none};
    }

    _metrics.resize(0);

    auto swMatches = FTDCBSONUtil::extractMetricsFromDocument(_referenceDoc, sample, &_metrics);

    if (!swMatches.isOK()) {
        return swMatches.getStatus();
    }

    dassert((swMatches.getValue() == false || _metricsCount == _metrics.size()) &&
            _metrics.size() < std::numeric_limits<std::uint32_t>::max());

    // We need to flush the current set of samples since the BSON schema has changed.
    if (!swMatches.getValue()) {
        auto swCompressedSamples = getCompressedSamples();

        if (!swCompressedSamples.isOK()) {
            return swCompressedSamples.getStatus();
        }

        // Set the new sample as the current reference document as we have to start all over
        _reset(sample, date);
        return {std::tuple<ConstDataRange, FTDCCompressor::CompressorState, Date_t>(
            std::get<0>(swCompressedSamples.getValue()),
            CompressorState::kSchemaChanged,
            std::get<1>(swCompressedSamples.getValue()))};
    }


    // Add another sample
    for (std::size_t i = 0; i < _metrics.size(); ++i) {
        // NOTE: This touches a lot of cache lines so that compression code can be more effcient.
        _deltas[getArrayOffset(_maxDeltas, _deltaCount, i)] = _metrics[i] - _prevmetrics[i];
    }

    ++_deltaCount;

    _prevmetrics.clear();
    swap(_prevmetrics, _metrics);

    // If the count is full, flush
    if (_deltaCount == _maxDeltas) {
        auto swCompressedSamples = getCompressedSamples();

        if (!swCompressedSamples.isOK()) {
            return swCompressedSamples.getStatus();
        }

        // Setup so that we treat the next sample as the reference sample
        _referenceDoc = BSONObj();

        return {std::tuple<ConstDataRange, FTDCCompressor::CompressorState, Date_t>(
            std::get<0>(swCompressedSamples.getValue()),
            CompressorState::kCompressorFull,
            std::get<1>(swCompressedSamples.getValue()))};
    }

    // The buffer is not full, inform the caller
    return {boost::none};
}

StatusWith<std::tuple<ConstDataRange, Date_t>> FTDCCompressor::getCompressedSamples() {
    _uncompressedChunkBuffer.setlen(0);

    // Append reference document - BSON Object
    _uncompressedChunkBuffer.appendBuf(_referenceDoc.objdata(), _referenceDoc.objsize());

    // Append count of metrics - uint32 little endian
    _uncompressedChunkBuffer.appendNum(static_cast<std::uint32_t>(_metricsCount));

    // Append count of samples - uint32 little endian
    _uncompressedChunkBuffer.appendNum(static_cast<std::uint32_t>(_deltaCount));

    if (_metricsCount != 0 && _deltaCount != 0) {
        // On average, we do not need all 10 bytes for every sample, worst case, we grow the buffer
        DataBuilder db(_metricsCount * _deltaCount * FTDCVarInt::kMaxSizeBytes64 / 2);

        std::uint32_t zeroesCount = 0;

        // For each set of samples for a particular metric,
        // we think of it is simple array of 64-bit integers we try to compress into a byte array.
        // This is done in three steps for each metric
        // 1. Delta Compression
        //   - i.e., we store the difference between pairs of samples, not their absolute values
        //   - this is done in addSamples
        // 2. Run Length Encoding of zeros
        //   - We find consecutive sets of zeros and represent them as a tuple of (0, count - 1).
        //   - Each memeber is stored as VarInt packed integer
        // 3. Finally, for non-zero members, we store these as VarInt packed
        //
        // These byte arrays are added to a buffer which is then concatenated with other chunks and
        // compressed with ZLIB.
        for (std::uint32_t i = 0; i < _metricsCount; i++) {
            for (std::uint32_t j = 0; j < _deltaCount; j++) {
                std::uint64_t delta = _deltas[getArrayOffset(_maxDeltas, j, i)];

                if (delta == 0) {
                    ++zeroesCount;
                    continue;
                }

                // If we have a non-zero sample, then write out all the accumulated zero samples.
                if (zeroesCount > 0) {
                    auto s1 = db.writeAndAdvance(FTDCVarInt(0));
                    if (!s1.isOK()) {
                        return s1;
                    }

                    auto s2 = db.writeAndAdvance(FTDCVarInt(zeroesCount - 1));
                    if (!s2.isOK()) {
                        return s2;
                    }

                    zeroesCount = 0;
                }

                auto s3 = db.writeAndAdvance(FTDCVarInt(delta));
                if (!s3.isOK()) {
                    return s3;
                }
            }

            // If we are on the last metric, and the previous loop ended in a zero, write out the
            // RLE
            // pair of zero information.
            if ((i == (_metricsCount - 1)) && zeroesCount) {
                auto s1 = db.writeAndAdvance(FTDCVarInt(0));
                if (!s1.isOK()) {
                    return s1;
                }

                auto s2 = db.writeAndAdvance(FTDCVarInt(zeroesCount - 1));
                if (!s2.isOK()) {
                    return s2;
                }
            }
        }

        // Append the entire compacted metric chunk into the uncompressed buffer
        ConstDataRange cdr = db.getCursor();
        _uncompressedChunkBuffer.appendBuf(cdr.data(), cdr.length());
    }

    auto swDest = _compressor.compress(
        ConstDataRange(_uncompressedChunkBuffer.buf(), _uncompressedChunkBuffer.len()));

    // The only way for compression to fail is if the buffer size calculations are wrong
    if (!swDest.isOK()) {
        return swDest.getStatus();
    }

    _compressedChunkBuffer.setlen(0);

    _compressedChunkBuffer.appendNum(static_cast<std::uint32_t>(_uncompressedChunkBuffer.len()));

    _compressedChunkBuffer.appendBuf(swDest.getValue().data(), swDest.getValue().length());

    return std::tuple<ConstDataRange, Date_t>(
        ConstDataRange(_compressedChunkBuffer.buf(),
                       static_cast<size_t>(_compressedChunkBuffer.len())),
        _referenceDocDate);
}

void FTDCCompressor::reset() {
    _metrics.clear();
    _reset(BSONObj(), Date_t());
}

void FTDCCompressor::_reset(const BSONObj& referenceDoc, Date_t date) {
    _referenceDoc = referenceDoc;
    _referenceDocDate = date;

    _metricsCount = _metrics.size();
    _deltaCount = 0;
    _prevmetrics.clear();
    swap(_prevmetrics, _metrics);

    // The reference document counts as the first sample, remaining samples
    // are delta encoded, so the maximum number of deltas is one less than
    // the configured number of samples.
    _maxDeltas = _config->maxSamplesPerArchiveMetricChunk - 1;
    _deltas.resize(_metricsCount * _maxDeltas);
}

}  // namespace mongo
