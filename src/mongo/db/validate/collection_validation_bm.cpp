// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/validate/collection_validation.h"

#include "mongo/base/init.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/clustered_collection_util.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/viewless_timeseries_collection_creation_helpers.h"
#include "mongo/db/timeseries/write_ops/timeseries_write_ops_utils.h"
#include "mongo/db/validate/validate_options.h"
#include "mongo/db/validate/validate_results.h"
#include "mongo/logv2/log_domain_global.h"
#include "mongo/logv2/log_manager.h"
#include "mongo/util/assert_util.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>

namespace mongo {
namespace {

const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("bench.validate");

// CatalogTestFixture is abstract (its base testing::Test has a pure-virtual TestBody()). The
// benchmark only needs setUp()/tearDown()/operationContext(), so provide a trivial concrete body.
class ValidateBenchmarkFixture : public CatalogTestFixture {
    void TestBody() override {}
};

// Validation logs verbosely ("validating collection", per-record diagnostics, etc.). Silence it so
// the benchmark measures work, not log I/O.
MONGO_INITIALIZER_GENERAL(DisableLoggingForCollectionValidationBM, (), ())
(InitializerContext*) {
    auto& lv2Manager = logv2::LogManager::global();
    logv2::LogDomainGlobal::ConfigurationOptions lv2Config;
    lv2Config.makeDisabled();
    uassertStatusOK(lv2Manager.getGlobalDomainInternal().configure(lv2Config));
}

// The shape of the collection to validate. Each axis is driven by a benchmark range arg so a single
// benchmark body can sweep records, document shape, and index count independently.
struct DataSpec {
    int numRecords{0};    // range(0): number of documents in the collection.
    int numFields{0};     // range(1): number of integer fields "a1".."aN" per document.
    int payloadBytes{0};  // range(2): size of an additional string field, in bytes (0 = omit).
    int numIndexes{0};    // range(3): number of secondary indexes, on fields "a1".."aK".
};

// Creates 'numIndexes' secondary indexes on the (empty) collection, keyed on fields a1..aK. Must be
// called before inserting any documents.
void createIndexes(OperationContext* opCtx, int numIndexes) {
    if (numIndexes == 0) {
        return;
    }
    std::vector<BSONObj> specs;
    for (int i = 1; i <= numIndexes; ++i) {
        const auto field = "a" + std::to_string(i);
        specs.push_back(BSON("v" << 2 << "name" << (field + "_1") << "key" << BSON(field << 1)));
    }
    uassertStatusOK(
        repl::StorageInterface::get(opCtx)->createIndexesOnEmptyCollection(opCtx, kNss, specs));
}

// Bulk-inserts spec.numRecords documents matching 'spec'. Returns the total BSON byte size inserted
// so the benchmark can report bytes/sec.
size_t insertRecords(OperationContext* opCtx, const DataSpec& spec) {
    constexpr int kBatchSize = 1000;
    const std::string payload(spec.payloadBytes, 'x');

    size_t totalBytes = 0;
    int inserted = 0;
    while (inserted < spec.numRecords) {
        const int batchCount = std::min(kBatchSize, spec.numRecords - inserted);
        std::vector<BSONObj> batch;
        batch.reserve(batchCount);
        for (int i = 0; i < batchCount; ++i) {
            const int id = inserted + i;
            BSONObjBuilder builder;
            builder.append("_id", id);
            for (int f = 1; f <= spec.numFields; ++f) {
                builder.append("a" + std::to_string(f), id * spec.numFields + f);
            }
            if (spec.payloadBytes > 0) {
                builder.append("p", payload);
            }
            auto obj = builder.obj();
            totalBytes += obj.objsize();
            batch.push_back(std::move(obj));
        }

        AutoGetCollection coll(opCtx, kNss, MODE_IX);
        WriteUnitOfWork wuow(opCtx);
        uassertStatusOK(Helpers::insert(opCtx, *coll, batch));
        wuow.commit();
        inserted += batchCount;
    }
    return totalBytes;
}

// Builds a fresh storage environment, populates a collection per 'state's range args, and times
// repeated collection_validation::validate() calls in the requested mode.
void BM_Validate(benchmark::State& state, collection_validation::ValidateMode mode) {
    const DataSpec spec{
        .numRecords = static_cast<int>(state.range(0)),
        .numFields = static_cast<int>(state.range(1)),
        .payloadBytes = static_cast<int>(state.range(2)),
        .numIndexes = static_cast<int>(state.range(3)),
    };

    ValidateBenchmarkFixture fixture;
    fixture.setUp();
    auto* opCtx = fixture.operationContext();

    uassertStatusOK(fixture.storageInterface()->createCollection(opCtx, kNss, CollectionOptions{}));
    createIndexes(opCtx, spec.numIndexes);
    const size_t totalBytes = insertRecords(opCtx, spec);

    for (auto _ : state) {
        ValidateResults results;
        uassertStatusOK(collection_validation::validate(
            opCtx,
            kNss,
            collection_validation::ValidationOptions{
                mode, collection_validation::RepairMode::kNone, /*logDiagnostics=*/false},
            &results));
        invariant(results.isValid());
        benchmark::DoNotOptimize(results);
    }

    state.SetItemsProcessed(state.iterations() * spec.numRecords);
    state.SetBytesProcessed(state.iterations() * totalBytes);

    fixture.tearDown();
}

// Baseline shape: 4 small fields, no payload, no secondary indexes. Sweeps record count.
void RecordCountArgs(benchmark::internal::Benchmark* b) {
    for (int n : {1'000, 10'000, 100'000}) {
        b->Args({n, 4, 0, 0});
    }
}

// Fixed record count; sweeps document size via a string payload field.
void DocSizeArgs(benchmark::internal::Benchmark* b) {
    for (int payload : {0, 256, 4'096, 16'384}) {
        b->Args({50'000, 4, payload, 0});
    }
}

// Fixed record count; sweeps the number of (small) fields per document, stressing BSON traversal.
void FieldCountArgs(benchmark::internal::Benchmark* b) {
    for (int fields : {1, 8, 32, 128}) {
        b->Args({50'000, fields, 0, 0});
    }
}

// Fixed record count; sweeps the number of secondary indexes (keyed on a1..aK; needs >= K fields).
void IndexCountArgs(benchmark::internal::Benchmark* b) {
    for (int indexes : {0, 1, 2, 4}) {
        b->Args({50'000, 4, 0, indexes});
    }
}

// Record-count scaling, across hash-off (kForeground), full, and hash-on (kCollectionHash) modes.
BENCHMARK_CAPTURE(BM_Validate,
                  Foreground_RecordCount,
                  collection_validation::ValidateMode::kForeground)
    ->Apply(RecordCountArgs);
BENCHMARK_CAPTURE(BM_Validate,
                  ForegroundFull_RecordCount,
                  collection_validation::ValidateMode::kForegroundFull)
    ->Apply(RecordCountArgs);
BENCHMARK_CAPTURE(BM_Validate,
                  CollectionHash_RecordCount,
                  collection_validation::ValidateMode::kCollectionHash)
    ->Apply(RecordCountArgs);

// Document-size axis, hash off vs on (where the per-record SHA256 cost scales with bytes).
BENCHMARK_CAPTURE(BM_Validate, Foreground_DocSize, collection_validation::ValidateMode::kForeground)
    ->Apply(DocSizeArgs);
BENCHMARK_CAPTURE(BM_Validate,
                  CollectionHash_DocSize,
                  collection_validation::ValidateMode::kCollectionHash)
    ->Apply(DocSizeArgs);

// Field-count axis (BSON-validation traversal cost per byte).
BENCHMARK_CAPTURE(BM_Validate,
                  Foreground_FieldCount,
                  collection_validation::ValidateMode::kForeground)
    ->Apply(FieldCountArgs);

// Index-count axis (full validation also traverses index keys against the record store).
BENCHMARK_CAPTURE(BM_Validate,
                  ForegroundFull_IndexCount,
                  collection_validation::ValidateMode::kForegroundFull)
    ->Apply(IndexCountArgs);

// ── Timeseries variant ────────────────────────────────────────────────────────

const NamespaceString kTsNss =
    NamespaceString::createNamespaceString_forTest("bench.system.buckets.validate_ts");
const NamespaceString kTsUserNss =
    NamespaceString::createNamespaceString_forTest("bench.validate_ts");

// One hour per bucket matches the default "seconds" granularity span.
constexpr int64_t kBucketSpanSeconds = 3600;

CollectionOptions makeTimeseriesOptions(const UUID& uuid) {
    CollectionOptions opts;
    opts.uuid = uuid;
    opts.timeseries = TimeseriesOptions("t");
    opts.timeseries->setGranularity(BucketGranularityEnum::Seconds);
    opts.timeseries->setBucketMaxSpanSeconds(kBucketSpanSeconds);
    opts.timeseries->setBucketRoundingSeconds(kBucketSpanSeconds);
    opts.clusteredIndex = clustered_util::makeCanonicalClusteredInfoForLegacyFormat();
    opts.validationAction = ValidationActionEnum::errorAndLog;
    opts.validator = timeseries::generateTimeseriesValidator(
        timeseries::kTimeseriesControlCompressedSortedVersion, "t");
    return opts;
}

// Inserts 'numBuckets' compressed bucket documents, each containing 'measurementsPerBucket'
// measurements spaced evenly within a one-hour window. Returns total BSON bytes inserted.
size_t insertTimeseriesBuckets(OperationContext* opCtx,
                               int numBuckets,
                               int measurementsPerBucket,
                               const CollectionOptions& opts) {
    const UUID uuid = *opts.uuid;
    const TimeseriesOptions& tsOpts = *opts.timeseries;

    // Base: 2024-01-01T00:00:00Z. Each bucket occupies a non-overlapping one-hour window.
    const int64_t baseMs = 1704067200000LL;
    const int64_t bucketSpanMs = kBucketSpanSeconds * 1000;
    const int64_t intervalMs = bucketSpanMs / measurementsPerBucket;

    constexpr int kBatchSize = 200;
    size_t totalBytes = 0;
    int inserted = 0;

    while (inserted < numBuckets) {
        const int batchCount = std::min(kBatchSize, numBuckets - inserted);
        std::vector<BSONObj> batch;
        batch.reserve(batchCount);

        for (int b = 0; b < batchCount; ++b) {
            const int64_t bucketBaseMs = baseMs + (inserted + b) * bucketSpanMs;
            std::vector<BSONObj> measurements;
            measurements.reserve(measurementsPerBucket);
            for (int m = 0; m < measurementsPerBucket; ++m) {
                measurements.push_back(
                    BSON("t" << Date_t::fromMillisSinceEpoch(bucketBaseMs + m * intervalMs) << "v"
                             << m));
            }
            auto bucketDoc = timeseries::write_ops::makeBucketDocument(
                measurements, kTsUserNss, uuid, tsOpts, nullptr);
            totalBytes += bucketDoc.objsize();
            batch.push_back(std::move(bucketDoc));
        }

        AutoGetCollection coll(opCtx, kTsNss, MODE_IX);
        WriteUnitOfWork wuow(opCtx);
        uassertStatusOK(Helpers::insert(opCtx, *coll, batch));
        wuow.commit();
        inserted += batchCount;
    }

    return totalBytes;
}

void BM_ValidateTimeseries(benchmark::State& state, collection_validation::ValidateMode mode) {
    const int numBuckets = static_cast<int>(state.range(0));
    const int measurementsPerBucket = static_cast<int>(state.range(1));

    const UUID uuid = UUID::gen();
    ValidateBenchmarkFixture fixture;
    fixture.setUp();
    auto* opCtx = fixture.operationContext();

    CollectionOptions opts = makeTimeseriesOptions(uuid);
    uassertStatusOK(fixture.storageInterface()->createCollection(opCtx, kTsNss, opts));
    const size_t totalBytes =
        insertTimeseriesBuckets(opCtx, numBuckets, measurementsPerBucket, opts);

    for (auto _ : state) {
        ValidateResults results;
        uassertStatusOK(collection_validation::validate(
            opCtx,
            kTsNss,
            collection_validation::ValidationOptions{
                mode, collection_validation::RepairMode::kNone, /*logDiagnostics=*/false},
            &results));
        invariant(results.isValid());
        benchmark::DoNotOptimize(results);
    }

    state.SetItemsProcessed(state.iterations() * numBuckets);
    state.SetBytesProcessed(state.iterations() * totalBytes);
    fixture.tearDown();
}

// Sweeps bucket count at fixed 10 measurements/bucket.
void TimeseriesBucketCountArgs(benchmark::internal::Benchmark* b) {
    for (int buckets : {1'000, 10'000, 50'000}) {
        b->Args({buckets, 10});
    }
}

// Sweeps measurements/bucket at fixed 10k buckets, stressing BSONColumn decode cost.
void TimeseriesMeasurementsPerBucketArgs(benchmark::internal::Benchmark* b) {
    for (int measurements : {1, 10, 50, 100}) {
        b->Args({10'000, measurements});
    }
}

BENCHMARK_CAPTURE(BM_ValidateTimeseries,
                  ForegroundFull_TS_BucketCount,
                  collection_validation::ValidateMode::kForegroundFull)
    ->Apply(TimeseriesBucketCountArgs);

BENCHMARK_CAPTURE(BM_ValidateTimeseries,
                  ForegroundFull_TS_MeasurementsPerBucket,
                  collection_validation::ValidateMode::kForegroundFull)
    ->Apply(TimeseriesMeasurementsPerBucketArgs);

}  // namespace
}  // namespace mongo
