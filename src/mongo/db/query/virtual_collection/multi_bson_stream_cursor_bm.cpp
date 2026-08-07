// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

/**
 * Measures the throughput of MultiBsonStreamCursor, the cursor that ingests BSON from an external
 * data source, across document sizes that exercise its different assembly-buffer paths:
 *   - Small documents: many per block read, so per-document overhead dominates. This is the case
 *     most sensitive to work added to the read loop.
 *   - Documents spanning block reads: the partial-object path.
 *   - Documents larger than the initial buffer: the buffer expansion path.
 */

#include "mongo/db/query/virtual_collection/multi_bson_stream_cursor.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/shard_role/shard_catalog/virtual_collection_options.h"
#include "mongo/transport/named_pipe/named_pipe.h"
#include "mongo/util/assert_util.h"

#include <cstdio>
#include <fstream>
#include <string>

#include <benchmark/benchmark.h>
#include <fmt/format.h>

namespace mongo {
namespace {

// Total bytes streamed per iteration, held constant across document sizes so that iterations remain
// comparable. A multiple of the largest document the cursor accepts, so that even the largest case
// below streams several documents and neither opening the file nor constructing the cursor is
// significant.
constexpr int64_t kBytesPerIteration = 4 * int64_t{BSONObjMaxUserSize};

// The smallest case below packs this many documents into a single block read, so that per-document
// overhead dominates.
constexpr int kTinyDocsPerBlockRead = 64;
constexpr int kTinyDocSize = MultiBsonStreamCursor::kInitialBlockReadSize / kTinyDocsPerBlockRead;
static_assert(MultiBsonStreamCursor::kInitialBlockReadSize % kTinyDocsPerBlockRead == 0,
              "a block read must hold a whole number of tiny documents");

/**
 * Returns a BSON document of exactly 'targetSize' bytes, padded out with a string field.
 */
BSONObj makeDocOfSize(int targetSize) {
    // The padding field's length prefix is a fixed-width int32, so the overhead of an otherwise
    // empty document does not vary with the padding's length. Measure it rather than restating it.
    static const int kOverhead = BSON("i" << 0 << "s" << "").objsize();
    invariant(targetSize >= kOverhead,
              fmt::format("document of {} bytes cannot hold its own {} bytes of overhead",
                          targetSize,
                          kOverhead));

    BSONObjBuilder builder;
    builder.append("i", 0);
    builder.append("s", std::string(targetSize - kOverhead, 'x'));
    BSONObj doc = builder.obj();
    invariant(doc.objsize() == targetSize,
              fmt::format("wanted {} bytes but built {}", targetSize, doc.objsize()));

    return doc;
}

/**
 * Writes 'count' copies of 'doc' into a file under the default pipe directory, where the cursor
 * resolves relative paths. Returns the file's name relative to that directory.
 */
std::string writeDocsFile(const BSONObj& doc, int64_t count) {
    static int64_t fileCounter = 0;
    const std::string relativePath = fmt::format("mbsc_bm_{}_{}", getpid(), ++fileCounter);
    const std::string absolutePath = std::string{kDefaultPipePath} + relativePath;

    std::ofstream out(absolutePath, std::ios::binary | std::ios::trunc);
    invariant(out.is_open(), fmt::format("failed to open {}", absolutePath));
    for (int64_t i = 0; i < count; ++i) {
        out.write(doc.objdata(), doc.objsize());
    }
    out.close();
    invariant(out.good(), fmt::format("failed to write {}", absolutePath));

    return relativePath;
}

VirtualCollectionOptions makeOptions(const std::string& relativePath) {
    VirtualCollectionOptions vopts;
    vopts.dataSources.emplace_back(ExternalDataSourceMetadata(
        fmt::format("{}{}", ExternalDataSourceMetadata::kUrlProtocolFile, relativePath),
        StorageTypeEnum::pipe,
        FileTypeEnum::bson));

    return vopts;
}

/**
 * Reads every document of 'docSize' bytes out of a pre-written file, once per iteration.
 */
void BM_ReadDocuments(benchmark::State& state) {
    const int docSize = state.range(0);
    const int64_t docCount = kBytesPerIteration / docSize;

    const BSONObj doc = makeDocOfSize(docSize);
    const std::string relativePath = writeDocsFile(doc, docCount);
    // '_vopts' is held by reference, so it must outlive every cursor built from it.
    const VirtualCollectionOptions vopts = makeOptions(relativePath);

    int64_t docsRead = 0;
    for (auto _ : state) {
        MultiBsonStreamCursor cursor(vopts);
        while (auto record = cursor.next()) {
            // next() itself cannot be optimized away, as it reads from the stream and mutates the
            // cursor, and the loop depends on its result. This keeps the whole record it assembled
            // live as well, so that no part of building the RecordId or RecordData is elided.
            benchmark::DoNotOptimize(record);
            ++docsRead;
        }
    }
    invariant(docsRead == docCount * static_cast<int64_t>(state.iterations()),
              fmt::format("read {} documents, expected {}", docsRead, docCount));

    state.SetItemsProcessed(docsRead);
    state.SetBytesProcessed(docsRead * docSize);

    std::remove((std::string{kDefaultPipePath} + relativePath).c_str());
}

BENCHMARK(BM_ReadDocuments)
    // 'kTinyDocsPerBlockRead' documents per block read, so per-document overhead dominates.
    ->Arg(kTinyDocSize)
    // One document per block read at the buffer's starting size.
    ->Arg(MultiBsonStreamCursor::kInitialBlockReadSize)
    // Several block reads per document, exercising the partial-object path.
    ->Arg(8 * MultiBsonStreamCursor::kInitialBufSize)
    // Far larger than the starting buffer, so the buffer must expand to its maximum.
    ->Arg(BSONObjMaxUserSize / 16)
    ->Arg(BSONObjMaxUserSize / 2)
    // The largest document that may be ingested.
    ->Arg(BSONObjMaxUserSize);

}  // namespace
}  // namespace mongo
