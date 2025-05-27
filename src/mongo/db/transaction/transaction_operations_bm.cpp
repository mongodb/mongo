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

#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/batched_write_context.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <benchmark/benchmark.h>

namespace mongo {

// First arg is the number of operations.  Second arg is the length of runs of contiguous statement
// IDs, or 0 for "all are contiguous".
void BM_AddOperations(benchmark::State& state) {
    const NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");

    auto runLength = state.range(1);
    auto nops = state.range(0);
    std::vector<UUID> uuids;
    auto doc = BSON("a" << 0);
    auto myId = BSON("_id" << 1);
    std::vector<repl::ReplOperation> ops;
    int skip = 0;
    for (int i = 0, run = 0; i < nops; i++, run++) {
        if (runLength && run == runLength) {
            skip++;
            run = 0;
        }
        auto op = repl::MutableOplogEntry::makeInsertOperation(nss, UUID::gen(), doc, myId);
        op.setInitializedStatementIds({i + skip});
        ops.emplace_back(std::move(op));
    }
    for (auto _ : state) {
        state.PauseTiming();
        TransactionOperations transactionOperations;
        state.ResumeTiming();
        for (const auto& op : ops)
            invariantStatusOK(transactionOperations.addOperation(op));
    }
}

BENCHMARK(BM_AddOperations)
    ->ArgsProduct({{1, 10, 100, 1000, 10000}, {0}})
    ->Args({10, 1})
    ->Args({100, 20})
    ->Args({1000, 50})
    ->Args({10000, 1000});
}  // namespace mongo
