/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/storage/sorted_data_interface.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/record_id.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/db/storage/sorted_data_interface_test_assert.h"
#include "mongo/db/storage/sorted_data_interface_test_harness.h"
#include "mongo/unittest/unittest.h"

#include <cstddef>
#include <memory>

#include <benchmark/benchmark.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

using Cursor = SortedDataInterface::Cursor;
enum Direction { kBackward, kForward };
enum Uniqueness { kUnique, kNonUnique };
enum EndPosition { kWithEnd, kWithoutEnd };
const auto kRecordId = Cursor::KeyInclusion::kExclude;
const auto kRecordIdAndKey = Cursor::KeyInclusion::kInclude;

struct Fixture {
    Fixture(Uniqueness uniqueness, Direction direction, int nToInsert, KeyFormat keyFormat)
        : uniqueness(uniqueness),
          direction(direction),
          nToInsert(nToInsert),
          harness(newSortedDataInterfaceHarnessHelper(1024)),
          opCtx(harness->newOperationContext()),
          sorted(harness->newSortedDataInterface(
              opCtx.get(), uniqueness == kUnique, /*partial*/ false, keyFormat)),
          cursor(sorted->newCursor(opCtx.get(),
                                   *shard_role_details::getRecoveryUnit(opCtx.get()),
                                   direction == kForward)),
          firstKey(makeKeyStringForSeek(sorted.get(),
                                        BSON("" << (direction == kForward ? 1 : nToInsert)),
                                        direction == kForward,
                                        true)) {

        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());

        StorageWriteTransaction txn(ru);
        for (int i = 0; i < nToInsert; i++) {
            BSONObj key = BSON("" << i);
            if (keyFormat == KeyFormat::Long) {
                RecordId loc(42, i * 2);
                ASSERT_SDI_INSERT_OK(
                    sorted->insert(opCtx.get(), ru, makeKeyString(sorted.get(), key, loc), true));
            } else {
                RecordId loc(record_id_helpers::keyForObj(key));
                ASSERT_SDI_INSERT_OK(sorted->insert(
                    opCtx.get(), ru, makeKeyString(sorted.get(), key, std::move(loc)), true));
            }
        }
        txn.commit();
        ASSERT_EQUALS(nToInsert, sorted->numEntries(opCtx.get(), ru));
    }

    const Uniqueness uniqueness;
    const Direction direction;
    const int nToInsert;

    std::unique_ptr<SortedDataInterfaceHarnessHelper> harness;
    ServiceContext::UniqueOperationContext opCtx;
    std::unique_ptr<SortedDataInterface> sorted;
    std::unique_ptr<SortedDataInterface::Cursor> cursor;
    key_string::Builder firstKey;
    size_t itemsProcessed = 0;
};

// Benchmark inserting a large number of entries into a WiredTiger table in fully sorted order using
// a bulk cursor.
void BM_SDIInsertionBulk(benchmark::State& state, int64_t numDocs) {
    std::unique_ptr<SortedDataInterfaceHarnessHelper> harness =
        newSortedDataInterfaceHarnessHelper();
    ServiceContext::UniqueOperationContext opCtx = harness->newOperationContext();
    std::unique_ptr<SortedDataInterface> sorted =
        harness->newSortedDataInterface(opCtx.get(), false, /*partial*/ false, KeyFormat::Long);

    for (auto _ : state) {
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
        auto builder = sorted->makeBulkBuilder(opCtx.get(), ru);

        int64_t numDocsInserted = 0;
        while (numDocsInserted < numDocs) {
            for (int i = 0; i < 1000; i++) {
                int64_t val = numDocsInserted * 1000 + i + 1;
                BSONObj key = BSON("" << val);
                RecordId loc(val);

                WriteUnitOfWork wunit(opCtx.get());
                builder->addKey(ru, makeKeyString(sorted.get(), key, loc));
                wunit.commit();
            }
            numDocsInserted += 1000;
            state.SetItemsProcessed(numDocsInserted);
        }
    }
};

// Benchmark inserting a large number of entries into a WiredTiger table in fully sorted order, in
// batches of 1000 docs using StorageWriteTransaction.
void BM_SDIInsertion(benchmark::State& state, int64_t numDocs) {
    std::unique_ptr<SortedDataInterfaceHarnessHelper> harness =
        newSortedDataInterfaceHarnessHelper();
    ServiceContext::UniqueOperationContext opCtx = harness->newOperationContext();
    std::unique_ptr<SortedDataInterface> sorted =
        harness->newSortedDataInterface(opCtx.get(), false, /*partial*/ false, KeyFormat::Long);

    for (auto _ : state) {
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());

        int64_t numDocsInserted = 0;
        while (numDocsInserted < numDocs) {
            StorageWriteTransaction txn(ru);
            for (int i = 0; i < 1000; i++) {
                int64_t val = numDocsInserted * 1000 + i + 1;
                BSONObj key = BSON("" << val);
                RecordId loc(val);
                ASSERT_SDI_INSERT_OK(
                    sorted->insert(opCtx.get(), ru, makeKeyString(sorted.get(), key, loc), true));
            }
            txn.commit();
            numDocsInserted += 1000;
            state.SetItemsProcessed(numDocsInserted);
        }
    }
};

// Benchmark inserting multiple sorted ranges into a WiredTiger table. This benchmark, compared to
// the one above, should inform our decision on whether and how to partition the collection scan
// phase during index builds.
void BM_SDIInsertionChunked(benchmark::State& state, int64_t numDocs, int numChunks) {
    std::unique_ptr<SortedDataInterfaceHarnessHelper> harness =
        newSortedDataInterfaceHarnessHelper();
    ServiceContext::UniqueOperationContext opCtx = harness->newOperationContext();
    std::unique_ptr<SortedDataInterface> sorted =
        harness->newSortedDataInterface(opCtx.get(), false, /*partial*/ false, KeyFormat::Long);

    for (auto _ : state) {
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());

        int64_t numDocsInserted = 0;
        int64_t chunkSize = numDocs / numChunks;
        for (int chunk = 0; chunk < numChunks; chunk++) {
            int64_t numBatches = chunkSize / 1000;
            for (int batch = 0; batch < numBatches; batch++) {
                StorageWriteTransaction txn(ru);
                for (int i = 0; i < 1000; i++) {
                    int64_t val = chunkSize * (batch * 1000 + i) + chunk + 1;
                    BSONObj key = BSON("" << val);
                    RecordId loc(val);
                    ASSERT_SDI_INSERT_OK(sorted->insert(
                        opCtx.get(), ru, makeKeyString(sorted.get(), key, loc), true));
                }
                txn.commit();

                numDocsInserted += 1000;
                state.SetItemsProcessed(numDocsInserted);
            }
        }
    }
};

void BM_SDIAdvance(benchmark::State& state,
                   Direction direction,
                   Cursor::KeyInclusion keyInclusion,
                   Uniqueness uniqueness,
                   KeyFormat keyFormat = KeyFormat::Long) {

    Fixture fix(uniqueness, direction, 100'000, keyFormat);

    auto& ru = *shard_role_details::getRecoveryUnit(fix.opCtx.get());
    for (auto _ : state) {
        fix.cursor->seek(ru, fix.firstKey.finishAndGetBuffer());
        for (int i = 1; i < fix.nToInsert; i++)
            fix.cursor->next(ru, keyInclusion);
        fix.itemsProcessed += fix.nToInsert;
    }
    ASSERT(!fix.cursor->next(ru));
    state.SetItemsProcessed(fix.itemsProcessed);
};

void BM_SDISeek(benchmark::State& state,
                Direction direction,
                Cursor::KeyInclusion keyInclusion,
                Uniqueness uniqueness) {

    Fixture fix(uniqueness, direction, 100'000, KeyFormat::Long);
    auto& ru = *shard_role_details::getRecoveryUnit(fix.opCtx.get());
    for (auto _ : state) {
        fix.cursor->seek(ru, fix.firstKey.finishAndGetBuffer(), keyInclusion);
        fix.itemsProcessed += 1;
    }
    state.SetItemsProcessed(fix.itemsProcessed);
};

void BM_SDISaveRestore(benchmark::State& state, Direction direction, Uniqueness uniqueness) {

    Fixture fix(uniqueness, direction, 100'000, KeyFormat::Long);

    auto& ru = *shard_role_details::getRecoveryUnit(fix.opCtx.get());
    for (auto _ : state) {
        fix.cursor->seek(ru, fix.firstKey.finishAndGetBuffer());
        fix.cursor->save();
        fix.cursor->restore(ru);
        fix.itemsProcessed += 1;
    }
    state.SetItemsProcessed(fix.itemsProcessed);
};

void BM_SDIAdvanceWithEnd(benchmark::State& state,
                          Direction direction,
                          Uniqueness uniqueness,
                          KeyFormat keyFormat = KeyFormat::Long) {

    Fixture fix(uniqueness, direction, 100'000, keyFormat);

    auto& ru = *shard_role_details::getRecoveryUnit(fix.opCtx.get());
    for (auto _ : state) {
        BSONObj lastKey = BSON("" << (direction == kForward ? fix.nToInsert : 1));
        fix.cursor->setEndPosition(lastKey, /*inclusive*/ true);
        fix.cursor->seek(ru, fix.firstKey.finishAndGetBuffer());
        for (int i = 1; i < fix.nToInsert; i++)
            fix.cursor->next(ru, kRecordId);
        fix.itemsProcessed += fix.nToInsert;
    }
    ASSERT(!fix.cursor->next(ru));
    state.SetItemsProcessed(fix.itemsProcessed);
};

BENCHMARK_CAPTURE(BM_SDIInsertionBulk, FullySorted, 10'000'000);
BENCHMARK_CAPTURE(BM_SDIInsertion, FullySorted, 10'000'000);
BENCHMARK_CAPTURE(BM_SDIInsertionChunked, With50Chunks, 10'000'000, 50);
BENCHMARK_CAPTURE(BM_SDIInsertionChunked, With10Chunks, 10'000'000, 10);
BENCHMARK_CAPTURE(BM_SDIInsertionChunked, With5Chunks, 10'000'000, 5);
BENCHMARK_CAPTURE(BM_SDIInsertionChunked, With2Chunks, 10'000'000, 2);

BENCHMARK_CAPTURE(BM_SDIAdvance, AdvanceForwardLoc, kForward, kRecordId, kNonUnique);
BENCHMARK_CAPTURE(BM_SDIAdvance, AdvanceForwardKeyAndLoc, kForward, kRecordIdAndKey, kNonUnique);
BENCHMARK_CAPTURE(BM_SDIAdvance, AdvanceForwardLocUnique, kForward, kRecordId, kUnique);
BENCHMARK_CAPTURE(BM_SDIAdvance, AdvanceForwardKeyAndLocUnique, kForward, kRecordIdAndKey, kUnique);
BENCHMARK_CAPTURE(
    BM_SDIAdvance, AdvanceForwardStringLoc, kForward, kRecordId, kNonUnique, KeyFormat::String);

BENCHMARK_CAPTURE(BM_SDIAdvance, AdvanceBackwardLoc, kBackward, kRecordId, kNonUnique);
BENCHMARK_CAPTURE(BM_SDIAdvance, AdvanceBackwardKeyAndLoc, kBackward, kRecordIdAndKey, kNonUnique);
BENCHMARK_CAPTURE(BM_SDIAdvance, AdvanceBackwardLocUnique, kBackward, kRecordId, kUnique);
BENCHMARK_CAPTURE(
    BM_SDIAdvance, AdvanceBackwardKeyAndLocUnique, kBackward, kRecordIdAndKey, kUnique);
BENCHMARK_CAPTURE(
    BM_SDIAdvance, AdvanceBackwardStringLoc, kForward, kRecordId, kNonUnique, KeyFormat::String);

BENCHMARK_CAPTURE(BM_SDISeek, SeekForwardKey, kForward, kRecordId, kNonUnique);
BENCHMARK_CAPTURE(BM_SDISeek, SeekForwardKeyAndLoc, kForward, kRecordIdAndKey, kNonUnique);
BENCHMARK_CAPTURE(BM_SDISeek, SeekForwardKeyUnique, kForward, kRecordId, kUnique);
BENCHMARK_CAPTURE(BM_SDISeek, SeekForwardKeyAndLocUnique, kForward, kRecordIdAndKey, kUnique);

BENCHMARK_CAPTURE(BM_SDISaveRestore, SaveRestore, kForward, kNonUnique);
BENCHMARK_CAPTURE(BM_SDISaveRestore, SaveRestoreUnique, kForward, kUnique);

BENCHMARK_CAPTURE(BM_SDIAdvanceWithEnd, AdvanceForward, kForward, kNonUnique);
BENCHMARK_CAPTURE(BM_SDIAdvanceWithEnd, AdvanceForwardUnique, kForward, kUnique);
BENCHMARK_CAPTURE(BM_SDIAdvanceWithEnd, AdvanceBackward, kBackward, kNonUnique);
BENCHMARK_CAPTURE(BM_SDIAdvanceWithEnd, AdvanceBackwardUnique, kBackward, kUnique);
BENCHMARK_CAPTURE(
    BM_SDIAdvanceWithEnd, AdvanceForwardStringLoc, kForward, kNonUnique, KeyFormat::String);

}  // namespace
}  // namespace mongo
