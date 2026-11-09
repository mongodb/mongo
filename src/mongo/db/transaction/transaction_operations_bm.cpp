// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/end_of_transaction_parameter.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/batched_write_context.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/session/logical_session_id_helpers.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

#include <limits>

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

// Measures building the applyOps oplog entries of an unprepared transaction commit, with the
// end-of-transaction write path off and on. Args are the number of insert operations, the size of
// each inserted document's payload, the number of operations per applyOps entry (0 for "all in
// one entry"), and whether the end-of-transaction write path is enabled, i.e. both the
// 'featureFlagEndOfTransactionChangeEventSynthesis' feature flag and the 'allowEndOfTransaction'
// server parameter are on.
void BM_LogOplogEntries(benchmark::State& state) {
    const auto numOps = state.range(0);
    const auto docBytes = state.range(1);
    const auto opsPerEntry = state.range(2);
    const bool eot = state.range(3);

    unittest::ServerParameterGuard allowEndOfTransactionGuard("allowEndOfTransaction", eot);
    unittest::ServerParameterGuard featureFlagGuard(
        "featureFlagEndOfTransactionChangeEventSynthesis", eot);
    invariant(isEndOfTransactionWritePathEnabled() == eot);

    const NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    const auto uuid = UUID::gen();
    const std::string payload(docBytes, 'x');
    TransactionOperations transactionOperations;
    for (int i = 0; i < numOps; i++) {
        auto doc = BSON("_id" << i << "x" << payload);
        invariantStatusOK(transactionOperations.addOperation(
            repl::MutableOplogEntry::makeInsertOperation(nss, uuid, doc, BSON("_id" << i))));
    }

    const auto info =
        transactionOperations.getApplyOpsInfo(opsPerEntry ? static_cast<std::size_t>(opsPerEntry)
                                                          : std::numeric_limits<std::size_t>::max(),
                                              static_cast<std::size_t>(BSONObjMaxUserSize),
                                              /*prepare=*/false);
    std::vector<OplogSlot> oplogSlots;
    for (std::size_t i = 0; i < info.numberOfOplogSlotsRequired; i++) {
        oplogSlots.emplace_back(Timestamp(1, i + 1), /*term=*/1LL);
    }
    const auto wallClockTime = Date_t::now();

    // Mirror the session fields the real commit path sets on each entry before serializing it, so
    // the entry sizes, and the marker's share of them, are realistic.
    const auto lsid = makeLogicalSessionIdForTest();
    const TxnNumber txnNumber{1};
    auto serialize = [&](repl::MutableOplogEntry* entry,
                         const std::vector<StmtId>& stmtIdsWritten) {
        entry->setSessionId(lsid);
        entry->setTxnNumber(txnNumber);
        if (!stmtIdsWritten.empty()) {
            entry->setStatementIds(stmtIdsWritten);
        }
        return entry->toBSON();
    };

    // Check once, untimed, that the marker is on exactly the final entry when the write path is
    // enabled and absent otherwise, so a misconfigured guard can't produce a misleading row. This
    // also measures the serialized size of the transaction's entries.
    std::size_t bytesPerTxn = 0;
    std::size_t numEntriesMarked = 0;
    bool lastEntryMarked = false;
    TransactionOperations::LogApplyOpsFn checkingLogApplyOpsFn =
        [&](repl::MutableOplogEntry* entry,
            bool firstOp,
            bool lastOp,
            std::vector<StmtId> stmtIdsWritten,
            WriteUnitOfWork::OplogEntryGroupType) {
            bytesPerTxn += serialize(entry, stmtIdsWritten).objsize();
            if (entry->getObject().getBoolField("allowEndOfTransaction")) {
                ++numEntriesMarked;
                lastEntryMarked = lastOp;
            }
            return entry->getOpTime();
        };
    boost::optional<TransactionOperations::TransactionOperation::ImageBundle> imageToWrite;
    const auto numEntries =
        transactionOperations.logOplogEntries(oplogSlots,
                                              info,
                                              wallClockTime,
                                              WriteUnitOfWork::noGroup,
                                              checkingLogApplyOpsFn,
                                              &imageToWrite,
                                              isEndOfTransactionWritePathEnabled());
    invariant(numEntriesMarked == (eot ? 1U : 0U), "unexpected number of marked applyOps entries");
    invariant(lastEntryMarked == eot, "marker is not on the final applyOps entry");

    TransactionOperations::LogApplyOpsFn logApplyOpsFn = [&](repl::MutableOplogEntry* entry,
                                                             bool firstOp,
                                                             bool lastOp,
                                                             std::vector<StmtId> stmtIdsWritten,
                                                             WriteUnitOfWork::OplogEntryGroupType) {
        benchmark::DoNotOptimize(serialize(entry, stmtIdsWritten));
        return entry->getOpTime();
    };
    for (auto _ : state) {
        boost::optional<TransactionOperations::TransactionOperation::ImageBundle> imageToWrite;
        benchmark::DoNotOptimize(
            transactionOperations.logOplogEntries(oplogSlots,
                                                  info,
                                                  wallClockTime,
                                                  WriteUnitOfWork::noGroup,
                                                  logApplyOpsFn,
                                                  &imageToWrite,
                                                  isEndOfTransactionWritePathEnabled()));
    }

    state.counters["bytes_per_txn"] = static_cast<double>(bytesPerTxn);
    state.counters["entries"] = static_cast<double>(numEntries);
}

BENCHMARK(BM_LogOplogEntries)
    ->ArgNames({"numOps", "docBytes", "opsPerEntry", "eot"})
    ->ArgsProduct({{1, 10, 100, 1000}, {32, 1024}, {0, 10}, {0, 1}});
}  // namespace mongo
