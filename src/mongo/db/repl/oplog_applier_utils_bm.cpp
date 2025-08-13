/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/repl/oplog_applier_utils.h"

#include "mongo/bson/timestamp.h"
#include "mongo/db/client.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/query/compiler/stats/rand_utils.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/service_context.h"
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
