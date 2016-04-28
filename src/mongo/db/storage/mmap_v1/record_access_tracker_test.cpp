/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/storage/mmap_v1/record_access_tracker.h"

#include "mongo/db/storage/mmap_v1/record.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"

using namespace mongo;

namespace {

const std::unique_ptr<ClockSource> clock = stdx::make_unique<ClockSourceMock>();

const void* pointerOf(int data) {
    return reinterpret_cast<const void*>(data);
}

TEST(RecordAccessTrackerTest, TouchRecordTwice) {
    RecordAccessTracker tracker(clock.get());
    tracker.disableSystemBlockInMemCheck();

    const void* record = pointerOf(0x10003);

    ASSERT_FALSE(tracker.checkAccessedAndMark(record));
    ASSERT_TRUE(tracker.checkAccessedAndMark(record));
}

TEST(RecordAccessTrackerTest, TouchPageTwice) {
    RecordAccessTracker tracker(clock.get());
    tracker.disableSystemBlockInMemCheck();

    const void* firstRecord = pointerOf(0x10003);
    const void* secondRecord = pointerOf(0x10004);

    ASSERT_FALSE(tracker.checkAccessedAndMark(firstRecord));
    ASSERT_TRUE(tracker.checkAccessedAndMark(secondRecord));
    ASSERT_TRUE(tracker.checkAccessedAndMark(firstRecord));
    ASSERT_TRUE(tracker.checkAccessedAndMark(secondRecord));
}

TEST(RecordAccessTrackerTest, TouchTwoPagesTwice) {
    RecordAccessTracker tracker(clock.get());
    tracker.disableSystemBlockInMemCheck();

    const void* firstRecordFirstPage = pointerOf(0x11000);
    const void* secondRecordFirstPage = pointerOf(0x11100);

    const void* firstRecordSecondPage = pointerOf(0x12000);
    const void* secondRecordSecondPage = pointerOf(0x12100);

    ASSERT_FALSE(tracker.checkAccessedAndMark(firstRecordFirstPage));
    ASSERT_FALSE(tracker.checkAccessedAndMark(firstRecordSecondPage));
    ASSERT_TRUE(tracker.checkAccessedAndMark(secondRecordFirstPage));
    ASSERT_TRUE(tracker.checkAccessedAndMark(secondRecordSecondPage));
}

// Tests RecordAccessTracker::reset().
TEST(RecordAccessTrackerTest, TouchTwoPagesTwiceWithReset) {
    RecordAccessTracker tracker(clock.get());
    tracker.disableSystemBlockInMemCheck();

    const void* firstRecordFirstPage = pointerOf(0x11000);
    const void* secondRecordFirstPage = pointerOf(0x11100);

    const void* firstRecordSecondPage = pointerOf(0x12000);
    const void* secondRecordSecondPage = pointerOf(0x12100);

    ASSERT_FALSE(tracker.checkAccessedAndMark(firstRecordFirstPage));
    ASSERT_FALSE(tracker.checkAccessedAndMark(firstRecordSecondPage));
    ASSERT_TRUE(tracker.checkAccessedAndMark(secondRecordFirstPage));
    ASSERT_TRUE(tracker.checkAccessedAndMark(secondRecordSecondPage));

    // Now reset and make sure things look as though we have a fresh RecordAccessTracker.
    tracker.reset();
    ASSERT_FALSE(tracker.checkAccessedAndMark(firstRecordFirstPage));
    ASSERT_FALSE(tracker.checkAccessedAndMark(firstRecordSecondPage));
    ASSERT_TRUE(tracker.checkAccessedAndMark(secondRecordFirstPage));
    ASSERT_TRUE(tracker.checkAccessedAndMark(secondRecordSecondPage));
}

// Tests RecordAccessTracker::markAccessed().
TEST(RecordAccessTrackerTest, AccessTest) {
    RecordAccessTracker tracker(clock.get());
    tracker.disableSystemBlockInMemCheck();

    // Mark the first page in superpage 3 as accessed.
    const void* record = pointerOf(0x30000);
    tracker.markAccessed(record);

    // Test that all remaining addresses in the page give true when asked whether they are
    // recently accessed.
    for (int i = 0x30001; i < 0x31000; i++) {
        const void* touchedPageRecord = pointerOf(i);
        ASSERT_TRUE(tracker.checkAccessedAndMark(touchedPageRecord));
    }
}

// Touch pages in 128 separate superpages, and make sure that they all are reported as
// recently accessed.
TEST(RecordAccessTrackerTest, Access128Superpages) {
    RecordAccessTracker tracker(clock.get());
    tracker.disableSystemBlockInMemCheck();

    // Touch the pages.
    for (int i = 0x00000; i < 0x800000; i += 0x10000) {
        const void* touchedPageRecord = pointerOf(i);
        tracker.markAccessed(touchedPageRecord);
    }

    // Ensure we know that the pages have all been touched.
    for (int i = 0x00000; i < 0x800000; i += 0x10000) {
        // It should be fine if there is an offset of, say, 0xA, into the page.
        const void* touchedPageRecord = pointerOf(i + 0xA);
        ASSERT_TRUE(tracker.checkAccessedAndMark(touchedPageRecord));
    }
}

}  // namespace
