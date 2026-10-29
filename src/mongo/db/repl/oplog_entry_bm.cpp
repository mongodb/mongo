// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/oplog_entry.h"

#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/apply_ops_command_info.h"
#include "mongo/db/session/logical_session_id_helpers.h"

#include <random>

#include <benchmark/benchmark.h>

namespace mongo {
namespace repl {

namespace {
BSONObj createOplogEntryWithNStatementIds(int numStmtIds) {
    const NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    const BSONObj oplogEntryWithNoStmtId =
        BSON("op" << "c"
                  << "ns" << nss.ns_forTest() << "o" << BSON("applyOps" << 1 << "_id" << 1) << "v"
                  << 2 << "ts" << Timestamp(0, 0) << "t" << 0LL << "wall" << Date_t());
    BSONObjBuilder bob(oplogEntryWithNoStmtId);
    if (numStmtIds == 1) {
        bob.append("stmtId", int32_t(99));
    } else if (numStmtIds > 1) {
        BSONArrayBuilder bab(bob.subarrayStart("stmtId"));
        for (int i = 0; i < numStmtIds; i++) {
            bab.append(int32_t(i + 101));
        }
        bab.done();
    }
    return bob.obj();
}
}  // namespace

void BM_ParseOplogEntryWithNStatementIds(benchmark::State& state) {
    auto numStmtIds = state.range();
    auto oplogEntryWithNStatementIds = createOplogEntryWithNStatementIds(numStmtIds);
    for (auto _ : state) {
        benchmark::DoNotOptimize(uassertStatusOK(OplogEntry::parse(oplogEntryWithNStatementIds)));
    }
}

void BM_AccessStatementIdsForOplogEntry(benchmark::State& state) {
    auto numStmtIds = state.range();
    auto oplogEntryWithNStatementIds = createOplogEntryWithNStatementIds(numStmtIds);
    auto oplogEntry = uassertStatusOK(OplogEntry::parse(oplogEntryWithNStatementIds));
    for (auto _ : state) {
        for (auto statementId : oplogEntry.getStatementIds()) {
            benchmark::DoNotOptimize(statementId);
        }
    }
}

// Getting the repl operation size should be quick, not dependent on the number of statement IDs.
void BM_GetDurableReplOperationSize(benchmark::State& state) {
    auto numStmtIds = state.range();
    auto oplogEntryWithNStatementIds = createOplogEntryWithNStatementIds(numStmtIds);
    auto oplogEntry = uassertStatusOK(OplogEntry::parse(oplogEntryWithNStatementIds));
    for (auto _ : state) {
        benchmark::DoNotOptimize(
            DurableOplogEntry::getDurableReplOperationSize(oplogEntry.getDurableReplOperation()));
    }
}

template <class T>
std::string genRandomString(T& engine, const int count) {
    std::string buf;
    std::uniform_int_distribution<int> char_dist(32, 94);
    buf.reserve(count);

    for (int i = 0; i < count; ++i) {
        buf.push_back(static_cast<char>(char_dist(engine)));
    }

    return buf;
}

BSONObj generateVectoredInsertApplyOps(size_t numEntries, size_t entrySize) {
    BSONObjBuilder applyOpsInnerBuilder;
    UUID someUUID = UUID::gen();
    std::default_random_engine e1;

    BSONArrayBuilder opsArrayBuilder(applyOpsInnerBuilder.subarrayStart("applyOps"));
    auto nss = NamespaceString::createNamespaceString_forTest("somedb.someColl");
    for (size_t i = 0; i < numEntries; i++) {
        auto oid = OID::gen();
        auto mystr = genRandomString(e1, entrySize);
        auto mydoc = BSON("_id" << oid << "s" << mystr);
        auto mydockey = BSON("_id" << oid);
        auto insertOp = MutableOplogEntry::makeInsertOperation(nss, someUUID, mydoc, mydockey);
        insertOp.setInitializedStatementIds({StmtId(i)});
        opsArrayBuilder.append(insertOp.toBSON());
    }
    opsArrayBuilder.done();
    BSONObj applyOpsInner = applyOpsInnerBuilder.obj();
    MutableOplogEntry applyOpsEntry;
    applyOpsEntry.setOpTime(OpTime(Timestamp(1281831, 12), 14));
    applyOpsEntry.setOpType(OpTypeEnum::kCommand);
    applyOpsEntry.setMultiOpType(MultiOplogEntryType::kApplyOpsAppliedSeparately);
    applyOpsEntry.setNss(NamespaceString::kAdminCommandNamespace);
    applyOpsEntry.setObject(applyOpsInner);
    applyOpsEntry.setSessionId(makeLogicalSessionIdForTest());
    applyOpsEntry.setTxnNumber(TxnNumber(15));
    applyOpsEntry.setWallClockTime(Date_t::fromDurationSinceEpoch(Days(13505)));
    return applyOpsEntry.toBSON().getOwned();
}

// The 'generateInsert' method should generate an insert equivalent to the internal operations in
// 'generateVectoredInsertApplyOps'.  The idea is to provide a benchmark to compare applyOps parsing
// against.
BSONObj generateInsert(size_t entrySize) {
    UUID someUUID = UUID::gen();
    std::default_random_engine e1;
    auto nss = NamespaceString::createNamespaceString_forTest("somedb.someColl");
    auto oid = OID::gen();
    auto mystr = genRandomString(e1, entrySize);
    auto mydoc = BSON("_id" << oid << "s" << mystr);
    auto mydockey = BSON("_id" << oid);

    MutableOplogEntry insertEntry;
    insertEntry.setObject(mydoc);
    insertEntry.setObject2(mydockey);
    insertEntry.setOpTime(OpTime(Timestamp(1281831, 12), 14));
    insertEntry.setOpType(OpTypeEnum::kInsert);
    insertEntry.setNss(nss);
    insertEntry.setUuid(someUUID);
    insertEntry.setSessionId(makeLogicalSessionIdForTest());
    insertEntry.setTxnNumber(TxnNumber(15));
    insertEntry.setWallClockTime(Date_t::fromDurationSinceEpoch(Days(13505)));
    return insertEntry.toBSON();
}

void BM_ApplyOpsExtractOperationsTo(benchmark::State& state) {
    auto numEntries = state.range(0);
    auto entrySize = state.range(1);

    BSONObj applyOps = generateVectoredInsertApplyOps(numEntries, entrySize);
    auto applyOpsEntry = uassertStatusOK(OplogEntry::parse(applyOps));
    for (auto _ : state) {
        std::vector<OplogEntry> operations;
        ApplyOps::extractOperationsTo(applyOpsEntry, applyOps, &operations);
        benchmark::DoNotOptimize(operations.size());
    }
}

void BM_NonApplyOpsParse(benchmark::State& state) {
    auto numEntries = state.range(0);
    auto entrySize = state.range(1);
    std::vector<BSONObj> insertOps;
    for (int i = 0; i < numEntries; i++) {
        insertOps.push_back(generateInsert(entrySize));
    }
    for (auto _ : state) {
        for (auto& insertOp : insertOps) {
            benchmark::DoNotOptimize(uassertStatusOK(OplogEntry::parse(insertOp)));
        }
    }
}

BENCHMARK(BM_ParseOplogEntryWithNStatementIds)->Arg(0)->Range(1, 1024);
BENCHMARK(BM_AccessStatementIdsForOplogEntry)->Arg(0)->Range(1, 1024);
// Since 1024 should be as fast as 0, we don't need to benchmark the intermediate values to see if
// something is wrong.  We just benchmark the three different cases of none, one, and many.
BENCHMARK(BM_GetDurableReplOperationSize)->Arg(0)->Arg(1)->Arg(1024);

constexpr std::initializer_list<int64_t> numsEntries{2, 64, 500};
constexpr std::initializer_list<int64_t> entrySizes{100};
BENCHMARK(BM_NonApplyOpsParse)->ArgsProduct({numsEntries, entrySizes});
BENCHMARK(BM_ApplyOpsExtractOperationsTo)->ArgsProduct({numsEntries, entrySizes});
}  // namespace repl
}  // namespace mongo
