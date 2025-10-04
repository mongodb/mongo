/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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
