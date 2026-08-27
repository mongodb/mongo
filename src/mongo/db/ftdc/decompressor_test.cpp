// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/ftdc/decompressor.h"

#include "mongo/base/data_range.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/ftdc/block_compressor.h"
#include "mongo/db/ftdc/ftdc_test.h"
#include "mongo/unittest/unittest.h"

#include <cstdint>
#include <string>
#include <vector>

namespace mongo {
namespace {

class FTDCDecompressorTest : public FTDCTest {};

/**
 * Manufactures a zlib-compressed FTDC metrics chunk with the provided metricsCount and sampleCount,
 * with no deltas afterward, then tries to decompress it. Used to validate that invalid combinations
 * of metricsCount + sampleCount are rejected by the FTDCDecompressor.
 */
StatusWith<std::vector<BSONObj>> uncompressChunkWithCounts(std::uint32_t metricsCount,
                                                           std::uint32_t sampleCount) {
    BSONObj ref = BSON("a" << 1);

    BufBuilder uncompressed;
    uncompressed.appendBuf(ref.objdata(), ref.objsize());
    uncompressed.appendNum(metricsCount);
    uncompressed.appendNum(sampleCount);

    BlockCompressor compressor;
    auto swCompressed = compressor.compress(
        ConstDataRange(uncompressed.buf(), static_cast<size_t>(uncompressed.len())));
    if (!swCompressed.isOK()) {
        return swCompressed.getStatus();
    }

    BufBuilder chunk;
    chunk.appendNum(static_cast<std::uint32_t>(uncompressed.len()));
    chunk.appendBuf(swCompressed.getValue().data(), swCompressed.getValue().length());

    FTDCDecompressor decompressor;
    return decompressor.uncompress(ConstDataRange(chunk.buf(), static_cast<size_t>(chunk.len())));
}

TEST_F(FTDCDecompressorTest, RejectsUncompressedLengthAboveLimit) {
    BufBuilder chunk;
    chunk.appendNum(static_cast<std::uint32_t>(FTDCDecompressor::kMaxDeltaFileSize + 1));

    FTDCDecompressor decompressor;
    auto sw =
        decompressor.uncompress(ConstDataRange(chunk.buf(), static_cast<size_t>(chunk.len())));

    ASSERT_EQ(sw.getStatus(), ErrorCodes::InvalidLength);
    ASSERT_STRING_CONTAINS(sw.getStatus().reason(),
                           "Metrics chunk has exceeded the allowable size.");
}

TEST_F(FTDCDecompressorTest, RejectsSampleCountAboveLimit) {
    // One metric, so the product equals sampleCount. sampleCount itself exceeds the cap.
    const auto sampleCount = static_cast<std::uint32_t>(FTDCDecompressor::kMaxTotalSamples + 1);
    auto sw = uncompressChunkWithCounts(1, sampleCount);

    ASSERT_EQ(sw.getStatus(), ErrorCodes::InvalidLength);
    ASSERT_STRING_CONTAINS(sw.getStatus().reason(),
                           std::to_string(FTDCDecompressor::kMaxTotalSamples));
}

TEST_F(FTDCDecompressorTest, RejectsMetricsTimesSamplesAboveLimit) {
    // Each factor is below kMaxTotalSamples, but the uint64 product is not.
    // 2^16 * 2^16 = 2^32, which also wraps to 0 if multiplied as uint32 — this would OOM
    // rather than fail this assertion if the decompressor used a wrapping multiply.
    const std::uint32_t metricsCount = 1u << 16;
    const std::uint32_t sampleCount = 1u << 16;
    auto sw = uncompressChunkWithCounts(metricsCount, sampleCount);

    ASSERT_EQ(sw.getStatus(), ErrorCodes::InvalidLength);
    ASSERT_STRING_CONTAINS(sw.getStatus().reason(),
                           std::to_string(FTDCDecompressor::kMaxTotalSamples));
}

}  // namespace
}  // namespace mongo
