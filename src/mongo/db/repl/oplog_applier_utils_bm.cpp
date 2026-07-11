// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/oplog_applier_utils.h"

#include "mongo/bson/timestamp.h"
#include "mongo/db/client.h"
#include "mongo/db/query/compiler/stats/rand_utils.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/recovery_unit_noop.h"

#include <chrono>
#include <string>
#include <vector>

#include <benchmark/benchmark.h>

namespace mongo {
namespace {

using repl::CachedCollectionProperties;
using repl::DurableOplogEntry;
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

}  // namespace
}  // namespace mongo
