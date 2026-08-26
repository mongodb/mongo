// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/oplog_applier_utils.h"

#include "mongo/bson/timestamp.h"
#include "mongo/db/client.h"
#include "mongo/db/query/compiler/stats/rand_utils.h"
#include "mongo/db/repl/container_oplog_entry_gen.h"
#include "mongo/db/repl/container_oplog_entry_serialization.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/recovery_unit_noop.h"

#include <chrono>
#include <span>
#include <string>
#include <vector>

#include <benchmark/benchmark.h>

namespace mongo {
namespace {

using repl::CachedCollectionProperties;
using repl::ContainerDeleteOplogEntryO;
using repl::ContainerInsertOplogEntryO;
using repl::ContainerKey;
using repl::ContainerVal;
using repl::DurableOplogEntry;
using repl::DurableOplogEntryParams;
using repl::OplogApplierUtils;
using repl::OplogEntry;
using repl::OpTime;
using repl::OpTypeEnum;

static const int32_t kSeed = 1;

ServiceContext* setupServiceContext() {
    // Disable server info logging so that the benchmark output is cleaner.
    logv2::LogManager::global().getGlobalSettings().setMinimumLoggedSeverity(
        mongo::logv2::LogComponent::kDefault, mongo::logv2::LogSeverity::Error());

    auto serviceContext = ServiceContext::make();
    auto serviceContextPtr = serviceContext.get();
    setGlobalServiceContext(std::move(serviceContext));

    return serviceContextPtr;
}

OplogEntry makeInsertOplogEntry(int t, const std::string& id, const NamespaceString& nss) {
    BSONObj oField = BSON("_id" << id << "x" << 1);
    return {DurableOplogEntry(OpTime(Timestamp(t, 1), 1),  // optime
                              OpTypeEnum::kInsert,         // op type
                              nss,                         // namespace
                              boost::none,                 // uuid
                              boost::none,                 // fromMigrate
                              boost::none,                 // checkExistenceForDiffInsert
                              boost::none,                 // versionContext
                              OplogEntry::kOplogVersion,   // version
                              oField,                      // o
                              boost::none,                 // o2
                              {},                          // sessionInfo
                              boost::none,                 // upsert
                              Date_t() + Seconds(t),       // wall clock time
                              {},                          // statement ids
                              boost::none,    // optime of previous write within same transaction
                              boost::none,    // pre-image optime
                              boost::none,    // post-image optime
                              boost::none,    // ShardId of resharding recipient
                              boost::none,    // _id
                              boost::none)};  // needsRetryImage
}

void BM_OplogEntryHash(benchmark::State& state) {
    auto serviceContext = setupServiceContext();
    ThreadClient threadClient(serviceContext->getService());
    auto opCtx = threadClient->makeOperationContext();

    shard_role_details::setRecoveryUnit(opCtx.get(),
                                        std::make_unique<RecoveryUnitNoop>(),
                                        WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);

    const auto kNumOps = 1000;
    const auto kDbName = "test";
    const auto kCollNameSize = 128;
    const auto kIdFieldSize = state.range(0);
    const auto kUnit = state.range(1);

    std::vector<OplogEntry> ops;
    ops.reserve(kNumOps);
    CachedCollectionProperties cache;

    // Generate the oplog entries, time is not measured.
    for (int i = 0; i < kNumOps; i++) {
        const auto idField = stats::genString(kIdFieldSize * kUnit, kSeed + i);
        const auto collName = stats::genString(kCollNameSize, kSeed + i);
        const auto nss = NamespaceString::createNamespaceString_forTest(kDbName, collName);
        cache.getCollectionProperties(opCtx.get(), nss);
        ops.push_back(makeInsertOplogEntry(i, idField, nss));
    }

    // Calculate the oplog entry hashes, time is automatically measured.
    for (auto _ : state) {
        for (int i = 0; i < kNumOps; i++) {
            benchmark::DoNotOptimize(
                OplogApplierUtils::getOplogEntryHash(opCtx.get(), &ops[i], &cache));
        }
    }
}

BENCHMARK(BM_OplogEntryHash)
    ->RangeMultiplier(4)
    ->Ranges({{1 << 2, 1 << 10}, {1, 1}})        // size of _id in bytes
    ->Ranges({{1 << 2, 1 << 10}, {1024, 1024}})  // size of _id in KB
    ->Unit(benchmark::kMillisecond);

/**
 * The four ways a container op can pack more than one key into a single oplog entry. Each takes a
 * different branch of the expansion, and they do not cost the same: the shared-value shape reuses
 * one value view for every key, while the paired shapes walk two arrays in lockstep.
 */
enum class PackedShape {
    // A single int key with an array of values, covering the consecutive keys starting at that key.
    kIntKeyArrayVals,
    // Equal-length arrays of keys and values, paired off.
    kArrayKeysArrayVals,
    // An array of keys sharing one value.
    kArrayKeysSharedVal,
    // A delete of an array of keys.
    kArrayKeysDelete,
};

constexpr std::string_view kContainerIdent = "internal-container-ident";
constexpr size_t kKeySize = 16;

std::span<const char> toSpan(const std::string& s) {
    return std::span<const char>{s.data(), s.size()};
}

std::vector<std::span<const char>> toSpans(const std::vector<std::string>& strings) {
    std::vector<std::span<const char>> spans;
    spans.reserve(strings.size());
    for (const auto& s : strings) {
        spans.push_back(toSpan(s));
    }
    return spans;
}

std::vector<std::string> genStrings(size_t n, size_t size) {
    std::vector<std::string> strings;
    strings.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        strings.push_back(stats::genString(size, kSeed + static_cast<int32_t>(i)));
    }
    return strings;
}

/**
 * Builds the 'o' field of a container op packing 'numKeys' keys in the given shape. The key and
 * value views only have to outlive the toBSON() call, which copies the bytes into the returned
 * BSONObj.
 */
[[nodiscard]] BSONObj makePackedContainerO(PackedShape shape, size_t numKeys, size_t valueSize) {
    if (shape == PackedShape::kArrayKeysDelete) {
        const auto keys = genStrings(numKeys, kKeySize);
        ContainerDeleteOplogEntryO o;
        o.setKey(ContainerKey(toSpans(keys)));
        return o.toBSON();
    }

    ContainerInsertOplogEntryO o;
    switch (shape) {
        case PackedShape::kIntKeyArrayVals: {
            const auto values = genStrings(numKeys, valueSize);
            o.setKey(ContainerKey(int64_t{1}));
            o.setValue(ContainerVal(toSpans(values)));
            return o.toBSON();
        }
        case PackedShape::kArrayKeysArrayVals: {
            const auto keys = genStrings(numKeys, kKeySize);
            const auto values = genStrings(numKeys, valueSize);
            o.setKey(ContainerKey(toSpans(keys)));
            o.setValue(ContainerVal(toSpans(values)));
            return o.toBSON();
        }
        case PackedShape::kArrayKeysSharedVal: {
            const auto keys = genStrings(numKeys, kKeySize);
            const auto value = stats::genString(valueSize, kSeed);
            o.setKey(ContainerKey(toSpans(keys)));
            o.setValue(ContainerVal(toSpan(value)));
            return o.toBSON();
        }
        case PackedShape::kArrayKeysDelete:
            MONGO_UNREACHABLE;
    }
    MONGO_UNREACHABLE;
}

OplogEntry makeContainerOplogEntry(int t, OpTypeEnum opType, const BSONObj& oField) {
    return {DurableOplogEntry(DurableOplogEntryParams{
        .opTime = OpTime(Timestamp(t, 1), 1),
        .opType = opType,
        .nss = NamespaceString::kContainerNamespace,
        .container = kContainerIdent,
        .version = OplogEntry::kOplogVersion,
        .oField = oField,
        .wallClockTime = Date_t() + Seconds(t),
    })};
}

OplogEntry makePackedContainerOplogEntry(int t,
                                         PackedShape shape,
                                         size_t numKeys,
                                         size_t valueSize) {
    const auto opType = shape == PackedShape::kArrayKeysDelete ? OpTypeEnum::kContainerDelete
                                                               : OpTypeEnum::kContainerInsert;
    return makeContainerOplogEntry(t, opType, makePackedContainerO(shape, numKeys, valueSize));
}

/**
 * Unrolls one packed container op into its single-key equivalents. This is the per-entry cost the
 * applier pays in the serial phase of _fillWriterVectors() before any writer thread runs, so it
 * scales with the number of keys the compact oplog format packed into the entry.
 */
void BM_ExpandBatchedContainerOp(benchmark::State& state, PackedShape shape) {
    const auto numKeys = static_cast<size_t>(state.range(0));
    const auto valueSize = static_cast<size_t>(state.range(1));

    const auto op = makePackedContainerOplogEntry(1, shape, numKeys, valueSize);

    // Guard against a shape that silently fails to pack, which would leave the benchmark measuring
    // a much smaller expansion than its parameters claim.
    const auto expectedSize = OplogApplierUtils::expandBatchedContainerOp(op);
    invariant(expectedSize && expectedSize->size() == numKeys);

    for (auto _ : state) {
        auto expanded = OplogApplierUtils::expandBatchedContainerOp(op);
        benchmark::DoNotOptimize(expanded->size());
    }

    state.SetItemsProcessed(state.iterations() * numKeys);
}

/**
 * Unrolls a whole applier batch. replBatchLimitOperations bounds the number of oplog entries in a
 * batch, not the number of keys they expand to, so holding the entry count fixed and varying
 * 'keysPerEntry' measures how much real work one batch grows to carry once entries are packed.
 */
void BM_ExpandBatchedContainerOpsBatch(benchmark::State& state, PackedShape shape) {
    const auto numEntries = static_cast<size_t>(state.range(0));
    const auto keysPerEntry = static_cast<size_t>(state.range(1));
    const auto valueSize = static_cast<size_t>(state.range(2));

    std::vector<OplogEntry> prototype;
    prototype.reserve(numEntries);
    for (size_t i = 0; i < numEntries; ++i) {
        prototype.push_back(
            makePackedContainerOplogEntry(static_cast<int>(i) + 1, shape, keysPerEntry, valueSize));
    }

    for (auto _ : state) {
        // Expansion replaces the batch in place, so each iteration needs a fresh copy. Copying an
        // OplogEntry is not free, so it is excluded from the measurement.
        state.PauseTiming();
        auto ops = prototype;
        state.ResumeTiming();

        OplogApplierUtils::expandBatchedContainerOps(ops);
        benchmark::DoNotOptimize(ops.size());
    }

    state.SetItemsProcessed(state.iterations() * numEntries * keysPerEntry);
}

/**
 * The same batch shape with nothing packed, which is what the applier sees when batched container
 * writes are off. Expansion still walks the batch to decide there is nothing to do, so this is the
 * floor the packed numbers should be compared against.
 */
void BM_ExpandBatchedContainerOpsUnpacked(benchmark::State& state) {
    const auto numEntries = static_cast<size_t>(state.range(0));
    const auto valueSize = static_cast<size_t>(state.range(1));

    std::vector<OplogEntry> prototype;
    prototype.reserve(numEntries);
    for (size_t i = 0; i < numEntries; ++i) {
        const auto value = stats::genString(valueSize, kSeed + static_cast<int32_t>(i));
        ContainerInsertOplogEntryO o;
        o.setKey(ContainerKey(static_cast<int64_t>(i)));
        o.setValue(ContainerVal(toSpan(value)));
        prototype.push_back(makeContainerOplogEntry(
            static_cast<int>(i) + 1, OpTypeEnum::kContainerInsert, o.toBSON()));
    }

    for (auto _ : state) {
        state.PauseTiming();
        auto ops = prototype;
        state.ResumeTiming();

        OplogApplierUtils::expandBatchedContainerOps(ops);
        benchmark::DoNotOptimize(ops.size());
    }

    state.SetItemsProcessed(state.iterations() * numEntries);
}

void addPackedOpArgs(benchmark::internal::Benchmark* bm) {
    bm->ArgNames({"keys", "valueBytes"});
    for (int64_t keys : {1, 8, 64, 512, 4096, 32768}) {
        for (int64_t valueBytes : {16, 128}) {
            bm->Args({keys, valueBytes});
        }
    }
}

void addBatchArgs(benchmark::internal::Benchmark* bm) {
    bm->ArgNames({"entries", "keysPerEntry", "valueBytes"});
    // 5000 entries is the default replBatchLimitOperations.
    for (int64_t keysPerEntry : {1, 8, 64, 512}) {
        bm->Args({5000, keysPerEntry, 16});
    }
}

BENCHMARK_CAPTURE(BM_ExpandBatchedContainerOp, intKeyArrayVals, PackedShape::kIntKeyArrayVals)
    ->Apply(addPackedOpArgs);
BENCHMARK_CAPTURE(BM_ExpandBatchedContainerOp, arrayKeysArrayVals, PackedShape::kArrayKeysArrayVals)
    ->Apply(addPackedOpArgs);
BENCHMARK_CAPTURE(BM_ExpandBatchedContainerOp, arrayKeysSharedVal, PackedShape::kArrayKeysSharedVal)
    ->Apply(addPackedOpArgs);
BENCHMARK_CAPTURE(BM_ExpandBatchedContainerOp, arrayKeysDelete, PackedShape::kArrayKeysDelete)
    ->Apply(addPackedOpArgs);

BENCHMARK_CAPTURE(BM_ExpandBatchedContainerOpsBatch, intKeyArrayVals, PackedShape::kIntKeyArrayVals)
    ->Apply(addBatchArgs)
    ->Unit(benchmark::kMillisecond);

BENCHMARK(BM_ExpandBatchedContainerOpsUnpacked)
    ->ArgNames({"entries", "valueBytes"})
    ->Args({5000, 16})
    ->Unit(benchmark::kMillisecond);

}  // namespace
}  // namespace mongo
