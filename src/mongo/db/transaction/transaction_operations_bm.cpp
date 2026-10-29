// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
