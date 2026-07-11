// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/document_value/document.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/document_value/document_internal.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/util/assert_util.h"

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <utility>

#include <benchmark/benchmark.h>


namespace mongo {
namespace {

/**
 * Generates a linearly-nested (and therefore unbalanced or skewed) BSON object with the given
 * number of non-object (leaf) fields, which is also the depth. For example, for numberOfLeaves = 3,
 * the result is
 * {
 *   "a": "AAA...",
 *   "b": {
 *     "a": "AAA...",
 *     "b": {
 *       "a": "AAA..."
 *     }
 *   }
 * }
 */
BSONObj generateSkewedBsonObj(size_t numberOfLeaves) {
    // Speed-up data generation by re-using previous result.
    static const std::string leafValue(128, 'A');
    static BSONObj result = BSON("a" << leafValue);
    static size_t resultSize = 1;

    invariant(numberOfLeaves >= 1);

    if (resultSize > numberOfLeaves) {
        // Reset if cannot re-use the previous result.
        result = BSON("a" << leafValue);
        resultSize = 1;
    }

    for (; resultSize < numberOfLeaves; ++resultSize) {
        result = BSON("a" << leafValue << "b" << result);
    }
    return result;
}

/**
 * Generates a flat BSON object with the given number of top-level string fields, each named
 * '<fieldPrefix><i>'. Unlike generateSkewedBsonObj(), this exercises the per-top-level-field work
 * in toBsonStrippingMetadata(), which checks every field name against isMetadataFieldName(). The
 * prefix controls which isMetadataFieldName() branch each field hits: a plain prefix short-circuits
 * on the leading-'$' check, while a '$' prefix forces the full set lookup (a miss, since these are
 * not real metadata names, so the fields are still serialized rather than stripped).
 */
BSONObj generateFlatBsonObj(size_t numberOfFields, std::string_view fieldPrefix) {
    static const std::string leafValue(128, 'A');
    BSONObjBuilder bb;
    for (size_t i = 0; i < numberOfFields; ++i) {
        bb.append(std::string{fieldPrefix} + std::to_string(i), leafValue);
    }
    return bb.obj();
}

}  // namespace

/**
 * Benchmarks document's serialization to BSON. The chosen method 'toBson(BSONObjBuilder*, size_t)'
 * by-passes trivial serialization (when document's storage is already in BSON format) and is called
 * by other serialization methods.
 */
void BM_documentToBson(benchmark::State& state) {
    Document doc{generateSkewedBsonObj(state.range(0))};
    for (auto _ : state) {
        BSONObjBuilder bb;
        doc.toBson(&bb);
    }
    state.counters["objsize"] = doc.toBson().objsize();
}

BENCHMARK(BM_documentToBson)->DenseRange(2'000, 10'000, 2'000)->Unit(benchmark::kMicrosecond);

void BM_FieldNameHasher(benchmark::State& state) {
    std::string field;
    for (auto i = 0; i < state.range(0); i++) {
        field.append("a");
    }

    for (auto _ : state) {
        benchmark::DoNotOptimize(FieldNameHasher{}(field));
    }
}

BENCHMARK(BM_FieldNameHasher)->RangeMultiplier(2)->Range(1, 1 << 8);

/**
 * Compares plain Document::toBson() against Document::toBsonStrippingMetadata() over the *same*
 * flat document, so the delta between the two isolates the per-top-level-field
 * isMetadataFieldName() scan that stripping adds. Both overloads serialize field-by-field
 * (toBson(BSONObjBuilder*) bypasses the trivial copy), so the only difference is the metadata
 * check. range(0): number of top-level fields. range(1): 0 = toBson() (no stripping), 1 =
 * toBsonStrippingMetadata(). range(2): 0 = plain field names (scan short-circuits on the
 * leading-'$' check), 1 = '$'-prefixed field names (scan does a full set lookup that misses).
 * Compare strip vs plain at the same field-name style: 'dollar' shows the worst-case per-field
 * scan cost, 'user' the common short-circuit case.
 */
void BM_documentToBsonStrippingMetadata(benchmark::State& state) {
    const bool strip = state.range(1);
    const bool dollarFields = state.range(2);
    state.SetLabel(std::string{dollarFields ? "dollar/" : "user/"} + (strip ? "strip" : "plain"));
    Document doc{generateFlatBsonObj(state.range(0), dollarFields ? "$field" : "field")};
    for (auto _ : state) {
        BSONObjBuilder bb;
        if (strip) {
            doc.toBsonStrippingMetadata(&bb);
        } else {
            doc.toBson(&bb);
        }
        BSONObj result = bb.obj();
        benchmark::DoNotOptimize(result);
    }
    state.counters["objsize"] = doc.toBson().objsize();
}

BENCHMARK(BM_documentToBsonStrippingMetadata)
    ->ArgsProduct({{1, 10, 50, 100, 500}, {0, 1}, {0, 1}})
    ->Unit(benchmark::kNanosecond);

void BM_isMetadataFieldName(benchmark::State& state) {
    std::string name;
    switch (state.range(0)) {
        case 0:
            name = "offering.startDate";
            state.SetLabel("user-field");
            break;
        case 1:
            name = std::string{Document::metaFieldSearchScore};
            state.SetLabel("metadata-hit");
            break;
        case 2:
            name = "$notAMetadataField";
            state.SetLabel("dollar-miss");
            break;
        default:
            MONGO_UNREACHABLE;
    }

    for (auto _ : state) {
        std::string_view fieldName{name};
        // Prevent the compiler from constant-folding the known input away.
        benchmark::DoNotOptimize(fieldName);
        bool result = Document::isMetadataFieldName(fieldName);
        benchmark::DoNotOptimize(result);
    }
}

BENCHMARK(BM_isMetadataFieldName)->Arg(0)->Arg(1)->Arg(2)->Unit(benchmark::kNanosecond);


}  // namespace mongo
