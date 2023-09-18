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

#include <benchmark/benchmark.h>

#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace repl {

namespace {
BSONObj createOplogEntryWithNStatementIds(int numStmtIds) {
    const NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    const BSONObj oplogEntryWithNoStmtId = BSON("op"
                                                << "c"
                                                << "ns" << nss.ns_forTest() << "o"
                                                << BSON("applyOps" << 1 << "_id" << 1) << "v" << 2
                                                << "ts" << Timestamp(0, 0) << "t" << 0LL << "wall"
                                                << Date_t());
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
    return bob.done();
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

BENCHMARK(BM_ParseOplogEntryWithNStatementIds)->Range(0, 1024);
BENCHMARK(BM_AccessStatementIdsForOplogEntry)->Range(0, 1024);
// Since 1024 should be as fast as 0, we don't need to benchmark the intermediate values to see if
// something is wrong.  We just benchmark the three different cases of none, one, and many.
BENCHMARK(BM_GetDurableReplOperationSize)->Arg(0)->Arg(1)->Arg(1024);
}  // namespace repl
}  // namespace mongo
