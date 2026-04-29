/**
 *    Copyright (C) 2026-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/replicated_fast_count/replicated_fast_count_delta_utils.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/replicated_fast_count/durable_size_metadata_gen.h"
#include "mongo/db/replicated_fast_count/replicated_fast_size_count.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>

namespace mongo {
namespace replicated_fast_count {
namespace {

// Minimal SeekableRecordCursor backed by an in-memory vector<BSONObj>. Ignores
// the seek bound and yields every record in order; aggregateSizeCountDeltasInOplog
// only needs forward iteration after a seek and never inspects the RecordIds.
class InMemoryOplogCursor : public SeekableRecordCursor {
public:
    explicit InMemoryOplogCursor(const std::vector<BSONObj>* entries) : _entries(entries) {}

    boost::optional<Record> seek(const RecordId&, BoundInclusion) override {
        _idx = 0;
        return next();
    }
    boost::optional<Record> seekExact(const RecordId&) override {
        return boost::none;
    }
    boost::optional<Record> next() override {
        if (_idx >= _entries->size()) {
            return boost::none;
        }
        const auto& obj = (*_entries)[_idx];
        Record rec{RecordId(static_cast<int64_t>(_idx + 1)),
                   RecordData(obj.objdata(), obj.objsize())};
        ++_idx;
        return rec;
    }
    void save() override {}
    bool restore(RecoveryUnit&, bool) override {
        return true;
    }
    void detachFromOperationContext() override {}
    void reattachToOperationContext(OperationContext*) override {}
    void setSaveStorageCursorOnDetachFromOperationContext(bool) override {}

private:
    const std::vector<BSONObj>* _entries;
    size_t _idx = 0;
};

NamespaceString userNss(int collIdx) {
    return NamespaceString::createNamespaceString_forTest("test.coll" + std::to_string(collIdx));
}

NamespaceString fastCountStoreNss() {
    return NamespaceString::makeGlobalConfigCollection(NamespaceString::kReplicatedFastCountStore);
}

repl::OplogEntrySizeMetadata makeSingleOpSizeMetadata(int32_t sz) {
    SingleOpSizeMetadata m;
    m.setSz(sz);
    return m;
}

// Builds a CRUD-style oplog entry (insert / update / delete) carrying an `m.sz` size delta.
BSONObj makeCrudEntry(int t,
                      repl::OpTypeEnum opType,
                      const NamespaceString& nss,
                      const UUID& uuid,
                      int32_t sz,
                      int payloadSize) {
    BSONObj o = [&] {
        BSONObjBuilder b;
        b.append("_id", t);
        if (payloadSize > 0) {
            b.append("d", std::string(payloadSize, 'x'));
        }
        return b.obj();
    }();
    BSONObj o2 = BSON("_id" << t);
    return repl::DurableOplogEntry{repl::DurableOplogEntryParams{
                                       .opTime = repl::OpTime(Timestamp(t, 1), 1),
                                       .opType = opType,
                                       .nss = nss,
                                       .uuid = uuid,
                                       .version = repl::DurableOplogEntry::kOplogVersion,
                                       .oField = o,
                                       .o2Field = o2,
                                       .sizeMetadata = makeSingleOpSizeMetadata(sz),
                                       .wallClockTime = Date_t::fromMillisSinceEpoch(0),
                                   }}
        .toBSON()
        .getOwned();
}

// Builds an inner applyOps op (the per-op shape that ApplyOps::extractOperationsTo
// sees). Reuses MutableOplogEntry::makeInsertOperation so the BSON layout matches
// what production transaction assembly produces.
BSONObj makeInnerInsertOpBson(
    const NamespaceString& nss, const UUID& uuid, int idVal, int32_t sz, int payloadSize) {
    BSONObj doc = [&] {
        BSONObjBuilder b;
        b.append("_id", idVal);
        if (payloadSize > 0) {
            b.append("d", std::string(payloadSize, 'x'));
        }
        return b.obj();
    }();
    auto op = repl::MutableOplogEntry::makeInsertOperation(nss, uuid, doc, BSON("_id" << idVal));
    op.setSizeMetadata(makeSingleOpSizeMetadata(sz));
    return op.toBSON();
}

// Builds an applyOps oplog entry whose `o.applyOps` array holds `inners`.
BSONObj makeApplyOpsEntry(int t, std::vector<BSONObj> inners) {
    BSONObj oField = [&] {
        BSONObjBuilder ob;
        BSONArrayBuilder arr(ob.subarrayStart("applyOps"));
        for (auto& inner : inners) {
            arr.append(inner);
        }
        arr.done();
        return ob.obj();
    }();
    return repl::DurableOplogEntry{repl::DurableOplogEntryParams{
                                       .opTime = repl::OpTime(Timestamp(t, 1), 1),
                                       .opType = repl::OpTypeEnum::kCommand,
                                       .nss = NamespaceString::kAdminCommandNamespace,
                                       .version = repl::DurableOplogEntry::kOplogVersion,
                                       .oField = oField,
                                       .wallClockTime = Date_t::fromMillisSinceEpoch(0),
                                   }}
        .toBSON()
        .getOwned();
}

// Builds a `c` (command) `create` oplog entry for a collection. Goes through the
// kCreate dispatch path in processOplogEntry.
BSONObj makeCreateEntry(int t, const NamespaceString& nss, const UUID& uuid) {
    return repl::DurableOplogEntry{repl::DurableOplogEntryParams{
                                       .opTime = repl::OpTime(Timestamp(t, 1), 1),
                                       .opType = repl::OpTypeEnum::kCommand,
                                       .nss = nss.getCommandNS(),
                                       .uuid = uuid,
                                       .version = repl::DurableOplogEntry::kOplogVersion,
                                       .oField = BSON("create" << nss.coll()),
                                       .wallClockTime = Date_t::fromMillisSinceEpoch(0),
                                   }}
        .toBSON()
        .getOwned();
}

// Builds a noop (`n`) oplog entry. No `m`, no `ui`. Skipped by the extraction logic
// at the size-metadata check.
BSONObj makeNoopEntry(int t) {
    return repl::DurableOplogEntry{
        repl::DurableOplogEntryParams{
            .opTime = repl::OpTime(Timestamp(t, 1), 1),
            .opType = repl::OpTypeEnum::kNoop,
            .nss = NamespaceString::createNamespaceString_forTest("test.$cmd"),
            .version = repl::DurableOplogEntry::kOplogVersion,
            .oField = BSON("msg" << "noop"),
            .wallClockTime = Date_t::fromMillisSinceEpoch(0),
        }}
        .toBSON()
        .getOwned();
}

// Builds a `c` (command) oplog entry whose first o-field is something we never
// count (createIndexes). Hits the dispatch table and gets skipped.
BSONObj makeUnrelatedCommandEntry(int t, const NamespaceString& nss) {
    return repl::DurableOplogEntry{
        repl::DurableOplogEntryParams{
            .opTime = repl::OpTime(Timestamp(t, 1), 1),
            .opType = repl::OpTypeEnum::kCommand,
            .nss = nss.getCommandNS(),
            .uuid = UUID::gen(),
            .version = repl::DurableOplogEntry::kOplogVersion,
            .oField =
                BSON("createIndexes" << nss.coll() << "v" << 2 << "key" << BSON("a" << 1) << "name"
                                     << "a_1"),
            .wallClockTime = Date_t::fromMillisSinceEpoch(0),
        }}
        .toBSON()
        .getOwned();
}

// ----- Workload generators -----

// Generates a realistic checkpoint-window shape: a pool of collections, each preceded
// by its `create` entry and followed by its insert quota. Mirrors what the metadata
// checkpoint thread observes (the create always precedes CRUD ops on a collection).
// Total entries returned is approximately `n` (off by at most kInsertsPerCollection).
std::vector<BSONObj> genAllInsertCRUD(int n, int payloadSize = 64) {
    constexpr int kInsertsPerCollection = 99;
    constexpr int kEntriesPerCollection = 1 + kInsertsPerCollection;  // 1 create + N inserts
    const int numCollections = std::max(1, n / kEntriesPerCollection);

    std::vector<BSONObj> out;
    out.reserve(n);
    int t = 1;
    for (int c = 0; c < numCollections; ++c) {
        const auto nss = userNss(c);
        const auto uuid = UUID::gen();
        out.push_back(makeCreateEntry(t++, nss, uuid));
        for (int j = 0; j < kInsertsPerCollection && static_cast<int>(out.size()) < n; ++j) {
            out.push_back(makeCrudEntry(t++,
                                        repl::OpTypeEnum::kInsert,
                                        nss,
                                        uuid,
                                        /*sz=*/200,
                                        payloadSize));
        }
    }
    return out;
}

std::vector<BSONObj> genMixedCRUD(int n) {
    std::vector<BSONObj> out;
    out.reserve(n);
    // Reuse a small pool of (nss, uuid) pairs so the deltas hashmap accumulates per UUID.
    constexpr int kCollections = 32;
    std::vector<std::pair<NamespaceString, UUID>> colls;
    colls.reserve(kCollections);
    for (int i = 0; i < kCollections; ++i) {
        colls.emplace_back(userNss(i), UUID::gen());
    }
    for (int i = 1; i <= n; ++i) {
        const auto& [nss, uuid] = colls[i % kCollections];
        repl::OpTypeEnum opType;
        int32_t sz;
        const int bucket = i % 10;
        if (bucket < 6) {
            opType = repl::OpTypeEnum::kInsert;
            sz = 200;
        } else if (bucket < 9) {
            opType = repl::OpTypeEnum::kUpdate;
            sz = 16;
        } else {
            opType = repl::OpTypeEnum::kDelete;
            sz = -200;
        }
        out.push_back(makeCrudEntry(i, opType, nss, uuid, sz, /*payloadSize=*/64));
    }
    return out;
}

// Sweep parameters. Powers of four from 1024 to 65536 give per-entry cost visible at
// scale (the plan's "~1k, 4k, 16k, 64k" data points).
constexpr int64_t kSweepStart = 1LL << 10;
constexpr int64_t kSweepEnd = 1LL << 16;
constexpr int kSweepMultiplier = 4;

// The applyOps benchmark sweeps on inner-op count with a fixed outer count, so the
// items/sec metric tracks per-inner-op throughput as transactions grow. Inner-op
// sweep is 16..1024 inner ops × 64 outer = 1k..64k inner ops processed.
constexpr int kApplyOpsOuterCount = 64;
constexpr int64_t kApplyOpsInnerSweepStart = 1LL << 4;
constexpr int64_t kApplyOpsInnerSweepEnd = 1LL << 10;

// Synthetic "transactional" applyOps. We do not set the prepare/commit fields because
// extractSizeCountDeltasForApplyOps drives off `o.applyOps` only.
std::vector<BSONObj> genTransactionalApplyOps(int numApplyOpsEntries, int innerOpsPerEntry) {
    std::vector<BSONObj> out;
    out.reserve(numApplyOpsEntries);
    constexpr int kCollections = 8;
    std::vector<std::pair<NamespaceString, UUID>> colls;
    colls.reserve(kCollections);
    for (int i = 0; i < kCollections; ++i) {
        colls.emplace_back(userNss(i), UUID::gen());
    }
    for (int i = 1; i <= numApplyOpsEntries; ++i) {
        std::vector<BSONObj> inners;
        inners.reserve(innerOpsPerEntry);
        for (int j = 0; j < innerOpsPerEntry; ++j) {
            const auto& [nss, uuid] = colls[(i + j) % kCollections];
            inners.push_back(makeInnerInsertOpBson(nss,
                                                   uuid,
                                                   /*idVal=*/j,
                                                   /*sz=*/200,
                                                   /*payloadSize=*/64));
        }
        out.push_back(makeApplyOpsEntry(i, std::move(inners)));
    }
    return out;
}

std::vector<BSONObj> genNoiseHeavy(int n) {
    std::vector<BSONObj> out;
    out.reserve(n);
    for (int i = 1; i <= n; ++i) {
        // ~80% noops, ~20% unrelated commands (createIndexes).
        if (i % 5 == 0) {
            out.push_back(makeUnrelatedCommandEntry(i, userNss(i % 16)));
        } else {
            out.push_back(makeNoopEntry(i));
        }
    }
    return out;
}

std::vector<BSONObj> genFastCountStoreWrites(int n) {
    std::vector<BSONObj> out;
    out.reserve(n);
    const auto storeNss = fastCountStoreNss();
    const UUID storeUuid = UUID::gen();
    for (int i = 1; i <= n; ++i) {
        out.push_back(makeCrudEntry(i,
                                    repl::OpTypeEnum::kInsert,
                                    storeNss,
                                    storeUuid,
                                    /*sz=*/0,
                                    /*payloadSize=*/0));
    }
    return out;
}

// Builds the typed input for the ReplOperation aggregate path.
std::vector<repl::ReplOperation> genReplOperations(int n) {
    std::vector<repl::ReplOperation> out;
    out.reserve(n);
    constexpr int kCollections = 32;
    std::vector<std::pair<NamespaceString, UUID>> colls;
    colls.reserve(kCollections);
    for (int i = 0; i < kCollections; ++i) {
        colls.emplace_back(userNss(i), UUID::gen());
    }
    for (int i = 0; i < n; ++i) {
        const auto& [nss, uuid] = colls[i % kCollections];
        auto op = repl::MutableOplogEntry::makeInsertOperation(
            nss, uuid, BSON("_id" << i), BSON("_id" << i));
        op.setSizeMetadata(makeSingleOpSizeMetadata(/*sz=*/200));
        out.push_back(std::move(op));
    }
    return out;
}

// ----- Benchmarks -----

// Sets items_per_second so that per-entry cost is directly comparable across sweep
// points. range(0) is always the primary sweep dimension (entry or outer-applyOps count).
void setItemsProcessed(benchmark::State& state) {
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * state.range(0));
}

void runScan(benchmark::State& state, const std::vector<BSONObj>& entries) {
    for (auto _ : state) {
        InMemoryOplogCursor cursor(&entries);
        auto result = aggregateSizeCountDeltasInOplog(cursor, Timestamp(0, 0), boost::none, false);
        benchmark::DoNotOptimize(result);
        benchmark::ClobberMemory();
    }
    setItemsProcessed(state);
}

void BM_Scan_AllInsertCRUD(benchmark::State& state) {
    const auto entries = genAllInsertCRUD(static_cast<int>(state.range(0)));
    runScan(state, entries);
}

// Same workload as BM_Scan_AllInsertCRUD but with a 4KB document body. The IDL
// parse cost grows with payload, so this variant exposes the per-byte savings of
// the BSON-view fast path more clearly than the small-payload baseline.
constexpr int kLargePayloadBytes = 4096;
void BM_Scan_AllInsertCRUD_LargePayload(benchmark::State& state) {
    const auto entries = genAllInsertCRUD(static_cast<int>(state.range(0)), kLargePayloadBytes);
    runScan(state, entries);
}

void BM_Scan_MixedCRUD(benchmark::State& state) {
    const auto entries = genMixedCRUD(static_cast<int>(state.range(0)));
    runScan(state, entries);
}

// Sweeps on inner-op count with a fixed outer applyOps count. items_per_second
// reports inner ops/sec, directly comparable to the non-applyOps CRUD benchmarks.
void BM_Scan_TransactionalApplyOps(benchmark::State& state) {
    const auto entries =
        genTransactionalApplyOps(kApplyOpsOuterCount, static_cast<int>(state.range(0)));
    for (auto _ : state) {
        InMemoryOplogCursor cursor(&entries);
        auto result = aggregateSizeCountDeltasInOplog(cursor, Timestamp(0, 0), boost::none, false);
        benchmark::DoNotOptimize(result);
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kApplyOpsOuterCount *
                            state.range(0));
}

void BM_Scan_NoiseHeavy(benchmark::State& state) {
    const auto entries = genNoiseHeavy(static_cast<int>(state.range(0)));
    runScan(state, entries);
}

void BM_Scan_FastCountStoreWrites(benchmark::State& state) {
    const auto entries = genFastCountStoreWrites(static_cast<int>(state.range(0)));
    runScan(state, entries);
}

void BM_AggregateMultiOp_ReplOperation(benchmark::State& state) {
    const auto ops = genReplOperations(static_cast<int>(state.range(0)));
    for (auto _ : state) {
        auto result = aggregateMultiOpSizeMetadata(ops);
        benchmark::DoNotOptimize(result);
        benchmark::ClobberMemory();
    }
    setItemsProcessed(state);
}

BENCHMARK(BM_Scan_AllInsertCRUD)->RangeMultiplier(kSweepMultiplier)->Range(kSweepStart, kSweepEnd);
BENCHMARK(BM_Scan_AllInsertCRUD_LargePayload)
    ->RangeMultiplier(kSweepMultiplier)
    ->Range(kSweepStart, kSweepEnd);
BENCHMARK(BM_Scan_MixedCRUD)->RangeMultiplier(kSweepMultiplier)->Range(kSweepStart, kSweepEnd);
BENCHMARK(BM_Scan_TransactionalApplyOps)
    ->RangeMultiplier(kSweepMultiplier)
    ->Range(kApplyOpsInnerSweepStart, kApplyOpsInnerSweepEnd);
BENCHMARK(BM_Scan_NoiseHeavy)->RangeMultiplier(kSweepMultiplier)->Range(kSweepStart, kSweepEnd);
BENCHMARK(BM_Scan_FastCountStoreWrites)
    ->RangeMultiplier(kSweepMultiplier)
    ->Range(kSweepStart, kSweepEnd);
BENCHMARK(BM_AggregateMultiOp_ReplOperation)
    ->RangeMultiplier(kSweepMultiplier)
    ->Range(kSweepStart, kSweepEnd);

}  // namespace
}  // namespace replicated_fast_count
}  // namespace mongo
