// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

/**
 * Benchmarks 'placeFieldsFromRecordInAccessors()', the per-document field extraction the collection
 * scan performs whenever the plan requests two or more fields. It walks every top-level field of
 * every document scanned, so its cost is paid once per field per document -- on a scan-heavy query
 * that is millions of calls per second.
 *
 * The field name length is the interesting axis: 'bson::fieldNameLength()' scans the first few
 * words inline before delegating to 'memchr()', so the size of the win depends on how long the
 * names are. The cases below bracket the 8-byte word size and go well past it, so a regression at
 * either end shows up rather than hiding behind an average.
 */

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/sbe/stages/scan_helpers.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/string_listset.h"

#include <string>
#include <vector>

#include <absl/container/inlined_vector.h>
#include <benchmark/benchmark.h>

namespace mongo::sbe {
namespace {

// Enough distinct documents that the walk cannot be hoisted out of the benchmark loop as
// loop-invariant.
constexpr size_t kNumDocs = 256;

constexpr size_t kFieldsPerDoc = 4;
constexpr size_t kRequestedFields = 3;

/**
 * Distinct field names of exactly 'nameLen' bytes. The trailing index keeps them unique; for
 * 'nameLen' of 1 that means single characters.
 */
std::vector<std::string> makeFieldNames(size_t nameLen, size_t count) {
    std::vector<std::string> names;
    for (size_t i = 0; i < count; ++i) {
        std::string suffix = std::to_string(i);
        invariant(nameLen >= suffix.size(), "field name length too small to stay unique");
        names.push_back(std::string(nameLen - suffix.size(), 'f') + suffix);
    }
    return names;
}

std::vector<BSONObj> makeDocs(const std::vector<std::string>& names) {
    std::vector<BSONObj> docs;
    for (size_t d = 0; d < kNumDocs; ++d) {
        BSONObjBuilder bob;
        for (size_t f = 0; f < names.size(); ++f) {
            // Vary the values, not the field names, so every document has an identical layout but
            // distinct contents.
            bob.appendNumber(names[f], static_cast<long long>(d * names.size() + f));
        }
        docs.push_back(bob.obj());
    }
    return docs;
}

void runExtraction(benchmark::State& state, const std::vector<std::string>& names) {
    auto docs = makeDocs(names);

    // Request the last 'kRequestedFields' names, so the walk has to pass over the earlier fields
    // before it can bail out early.
    std::vector<std::string> requested(names.end() - kRequestedFields, names.end());
    StringListSet scanFieldNames{requested};

    absl::InlinedVector<value::OwnedValueAccessor, 4> accessors;
    accessors.resize(requested.size());

    size_t idx = 0;
    size_t fieldsExtracted = 0;
    for (auto _ : state) {
        const BSONObj& obj = docs[idx];
        idx = (idx + 1) % kNumDocs;

        Record record{RecordId{static_cast<int64_t>(idx)},
                      RecordData{obj.objdata(), obj.objsize()}};
        benchmark::DoNotOptimize(record);

        placeFieldsFromRecordInAccessors(record, scanFieldNames, accessors);

        for (auto& accessor : accessors) {
            auto tagValue = accessor.getViewOfValue();
            benchmark::DoNotOptimize(tagValue);
        }
        benchmark::ClobberMemory();
        fieldsExtracted += accessors.size();
    }
    state.SetItemsProcessed(static_cast<int64_t>(fieldsExtracted));
}

void BM_FieldExtractionByNameLength(benchmark::State& state) {
    const auto nameLen = static_cast<size_t>(state.range(0));
    runExtraction(state, makeFieldNames(nameLen, kFieldsPerDoc));
}

// Bracket the 8-byte word size that 'fieldNameLength()' scans in, then keep going well past it.
// The large sizes matter because libc 'strlen()' is vectorized (16 bytes per iteration via NEON on
// aarch64, 32 via AVX2 on x86-64) against 8 bytes per iteration here, so it should overtake the
// inline scan once names get long enough. These cases are what locate that crossover.
BENCHMARK(BM_FieldExtractionByNameLength)
    ->Arg(1)
    ->Arg(4)
    ->Arg(8)
    ->Arg(9)
    ->Arg(12)
    ->Arg(16)
    ->Arg(29)
    ->Arg(64)
    ->Arg(128)
    ->Arg(256);

void BM_FieldExtractionWorkloadShape(benchmark::State& state) {
    runExtraction(state, {"_id", "zero", "hello", "randomInt"});
}
BENCHMARK(BM_FieldExtractionWorkloadShape);

}  // namespace
}  // namespace mongo::sbe
