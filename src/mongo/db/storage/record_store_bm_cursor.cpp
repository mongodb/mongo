// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/timestamp.h"
#include "mongo/db/record_id.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/record_store_test_harness.h"
#include "mongo/logv2/log_domain_global.h"
#include "mongo/unittest/unittest.h"

#include <benchmark/benchmark.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

MONGO_INITIALIZER_GENERAL(DisableLogging, (), ())
(InitializerContext*) {
    auto& lv2Manager = logv2::LogManager::global();
    logv2::LogDomainGlobal::ConfigurationOptions lv2Config;
    lv2Config.makeDisabled();
    uassertStatusOK(lv2Manager.getGlobalDomainInternal().configure(lv2Config));
}

enum Direction { kBackward, kForward };
const auto kIncludeBound = SeekableRecordCursor::BoundInclusion::kInclude;
const auto kExcludeBound = SeekableRecordCursor::BoundInclusion::kExclude;

struct Fixture {
    Fixture(Direction direction, int nToInsert, bool capped = false)
        : nToInsert(nToInsert),
          harness(newRecordStoreHarnessHelper()),
          rs(harness->newRecordStore("ns", RecordStore::Options{.isCapped = capped})),
          opCtx(harness->newOperationContext()),
          cursor(rs->getCursor(opCtx.get(),
                               *shard_role_details::getRecoveryUnit(opCtx.get()),
                               direction == kForward)) {
        char data[] = "data";
        int inserted = 0;
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
        while (inserted < nToInsert) {
            StorageWriteTransaction txn(ru);
            for (int i = 0; i < 100; i++) {
                ASSERT_OK(rs->insertRecord(opCtx.get(),
                                           *shard_role_details::getRecoveryUnit(opCtx.get()),
                                           data,
                                           strlen(data),
                                           Timestamp()));
                inserted++;
            }
            txn.commit();
        }
        ASSERT_EQUALS(nToInsert, rs->numRecords());
    }

    const int nToInsert;
    std::unique_ptr<RecordStoreHarnessHelper> harness;
    std::unique_ptr<RecordStore> rs;
    ServiceContext::UniqueOperationContext opCtx;
    std::unique_ptr<SeekableRecordCursor> cursor;
    size_t itemsProcessed = 0;
};

void BM_RecordStoreSeek(benchmark::State& state,
                        Direction direction,
                        SeekableRecordCursor::BoundInclusion boundInclusion) {
    Fixture fix(direction, 100'000);
    auto& ru = *shard_role_details::getRecoveryUnit(fix.opCtx.get());
    for (auto _ : state) {
        fix.cursor->seek(RecordId(50'000), boundInclusion);

        state.PauseTiming();
        fix.itemsProcessed += 1;
        fix.cursor->saveUnpositioned();
        fix.cursor->restore(ru);
        state.ResumeTiming();
    }
    state.SetItemsProcessed(fix.itemsProcessed);
};

void BM_RecordStoreMultiSeek(benchmark::State& state,
                             Direction direction,
                             SeekableRecordCursor::BoundInclusion boundInclusion) {
    Fixture fix(direction, 100'000);
    for (auto _ : state) {
        fix.cursor->seek(RecordId(1), boundInclusion);
        fix.cursor->seek(RecordId(50'000), boundInclusion);
        fix.cursor->seek(RecordId(100'000), boundInclusion);
        fix.itemsProcessed += 3;
    }
    state.SetItemsProcessed(fix.itemsProcessed);
};

void BM_RecordStoreSeekExact(benchmark::State& state, Direction direction) {
    Fixture fix(direction, 100'000);
    for (auto _ : state) {
        fix.cursor->seekExact(RecordId(50'000));
        fix.itemsProcessed += 1;
    }
    state.SetItemsProcessed(fix.itemsProcessed);
};

void BM_RecordStoreMultiSeekExact(benchmark::State& state, Direction direction) {
    Fixture fix(direction, 100'000);
    for (auto _ : state) {
        fix.cursor->seekExact(RecordId(1));
        fix.cursor->seekExact(RecordId(50'000));
        fix.cursor->seekExact(RecordId(100'000));
        fix.itemsProcessed += 3;
    }
    state.SetItemsProcessed(fix.itemsProcessed);
};

void BM_RecordStoreAdvance(benchmark::State& state, Direction direction) {
    Fixture fix(direction, 100'000);
    int start;
    if (direction == kBackward) {
        start = fix.nToInsert;
    } else {
        start = 1;
    }
    for (auto _ : state) {
        fix.cursor->seekExact(RecordId(start));
        for (int i = 1; i < fix.nToInsert; i++)
            ASSERT(fix.cursor->next());
        fix.itemsProcessed += fix.nToInsert;
    }
    ASSERT(!fix.cursor->next());
    state.SetItemsProcessed(fix.itemsProcessed);
};

void BM_RecordStoreSaveRestore(benchmark::State& state) {
    Fixture fix(kForward, 100'000);
    auto& ru = *shard_role_details::getRecoveryUnit(fix.opCtx.get());
    for (auto _ : state) {
        fix.cursor->seekExact(RecordId(1));
        fix.cursor->save();
        fix.cursor->restore(ru);
        fix.itemsProcessed += 1;
    }
    state.SetItemsProcessed(fix.itemsProcessed);
};

BENCHMARK_CAPTURE(BM_RecordStoreSeek, SeekForwardIncludeBound, kForward, kIncludeBound);
BENCHMARK_CAPTURE(BM_RecordStoreSeek, SeekForwardExcludeBound, kForward, kExcludeBound);
BENCHMARK_CAPTURE(BM_RecordStoreSeek, SeekBackwardIncludeBound, kBackward, kIncludeBound);
BENCHMARK_CAPTURE(BM_RecordStoreSeek, SeekBackwardExcludeBound, kBackward, kExcludeBound);

BENCHMARK_CAPTURE(BM_RecordStoreMultiSeek, MultiSeekForwardIncludeBound, kForward, kIncludeBound);
BENCHMARK_CAPTURE(BM_RecordStoreMultiSeekExact, MultiSeekExactForward, kForward);

BENCHMARK_CAPTURE(BM_RecordStoreSeekExact, SeekExactForward, kForward);
BENCHMARK_CAPTURE(BM_RecordStoreSeekExact, SeekExactBackward, kBackward);

BENCHMARK_CAPTURE(BM_RecordStoreAdvance, AdvanceForward, kForward);
BENCHMARK_CAPTURE(BM_RecordStoreAdvance, AdvanceBackward, kBackward);

BENCHMARK(BM_RecordStoreSaveRestore);

}  // namespace
}  // namespace mongo
