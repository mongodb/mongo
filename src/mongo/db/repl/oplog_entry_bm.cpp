/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/repl/oplog_entry.h"

#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/apply_ops_command_info.h"

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
