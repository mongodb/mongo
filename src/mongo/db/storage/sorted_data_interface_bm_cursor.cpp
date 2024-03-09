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


#include <benchmark/benchmark.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <cstddef>
#include <memory>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/record_id.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/db/storage/sorted_data_interface_test_harness.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/unittest/assert.h"

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
          harness(newSortedDataInterfaceHarnessHelper()),
          sorted(
              harness->newSortedDataInterface(uniqueness == kUnique, /*partial*/ false, keyFormat)),
          opCtx(harness->newOperationContext()),
          globalLock(opCtx.get(), MODE_X),
          cursor(sorted->newCursor(opCtx.get(), direction == kForward)),
          firstKey(makeKeyStringForSeek(sorted.get(),
                                        BSON("" << (direction == kForward ? 1 : nToInsert)),
                                        direction == kForward,
                                        true)) {

        WriteUnitOfWork uow(opCtx.get());
        for (int i = 0; i < nToInsert; i++) {
            BSONObj key = BSON("" << i);
            if (keyFormat == KeyFormat::Long) {
                RecordId loc(42, i * 2);
                ASSERT_OK(sorted->insert(opCtx.get(), makeKeyString(sorted.get(), key, loc), true));
            } else {
                RecordId loc(record_id_helpers::keyForObj(key));
                ASSERT_OK(sorted->insert(
                    opCtx.get(), makeKeyString(sorted.get(), key, std::move(loc)), true));
            }
        }
        uow.commit();
        ASSERT_EQUALS(nToInsert, sorted->numEntries(opCtx.get()));
    }

    const Uniqueness uniqueness;
    const Direction direction;
    const int nToInsert;

    std::unique_ptr<SortedDataInterfaceHarnessHelper> harness;
    std::unique_ptr<SortedDataInterface> sorted;
    ServiceContext::UniqueOperationContext opCtx;
    Lock::GlobalLock globalLock;
    std::unique_ptr<SortedDataInterface::Cursor> cursor;
    key_string::Value firstKey;
    size_t itemsProcessed = 0;
};

void BM_SDIAdvance(benchmark::State& state,
                   Direction direction,
                   Cursor::KeyInclusion keyInclusion,
                   Uniqueness uniqueness,
                   KeyFormat keyFormat = KeyFormat::Long) {

    Fixture fix(uniqueness, direction, 100'000, keyFormat);

    for (auto _ : state) {
        fix.cursor->seek(fix.firstKey);
        for (int i = 1; i < fix.nToInsert; i++)
            fix.cursor->next(keyInclusion);
        fix.itemsProcessed += fix.nToInsert;
    }
    ASSERT(!fix.cursor->next());
    state.SetItemsProcessed(fix.itemsProcessed);
};

void BM_SDISeek(benchmark::State& state,
                Direction direction,
                Cursor::KeyInclusion keyInclusion,
                Uniqueness uniqueness) {

    Fixture fix(uniqueness, direction, 100'000, KeyFormat::Long);
    for (auto _ : state) {
        fix.cursor->seek(fix.firstKey, keyInclusion);
        fix.itemsProcessed += 1;
    }
    state.SetItemsProcessed(fix.itemsProcessed);
};

void BM_SDISaveRestore(benchmark::State& state, Direction direction, Uniqueness uniqueness) {

    Fixture fix(uniqueness, direction, 100'000, KeyFormat::Long);

    for (auto _ : state) {
        fix.cursor->seek(fix.firstKey);
        fix.cursor->save();
        fix.cursor->restore();
        fix.itemsProcessed += 1;
    }
    state.SetItemsProcessed(fix.itemsProcessed);
};

void BM_SDIAdvanceWithEnd(benchmark::State& state,
                          Direction direction,
                          Uniqueness uniqueness,
                          KeyFormat keyFormat = KeyFormat::Long) {

    Fixture fix(uniqueness, direction, 100'000, keyFormat);

    for (auto _ : state) {
        BSONObj lastKey = BSON("" << (direction == kForward ? fix.nToInsert : 1));
        fix.cursor->setEndPosition(lastKey, /*inclusive*/ true);
        fix.cursor->seek(fix.firstKey);
        for (int i = 1; i < fix.nToInsert; i++)
            fix.cursor->next(kRecordId);
        fix.itemsProcessed += fix.nToInsert;
    }
    ASSERT(!fix.cursor->next());
    state.SetItemsProcessed(fix.itemsProcessed);
};


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
