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

#include "mongo/db/index/column_store_sorter.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
TEST(ColumnStoreSorter, SortTest) {
    // Each entry of the top-level vector contains the field names of a sample document whose
    // RecordId is the entry index. No field values are included in this sample data.
    std::vector<std::vector<std::string>> sampleData = {{"foo", "bar", "foo.bar", "bar.bar"},
                                                        {"bar", "foo", "bar.bar"},
                                                        {"bar.bar", "foo.bar", "bar", "foo"},
                                                        {"bar", "foo.bar", "baz"},
                                                        {"foo.bar", "bar", "foo"}};

    // The output of sorting the 'sampleData' field names by (Field name, RecordId).
    std::vector<std::pair<StringData, int64_t>> sortedData = {{"bar", 0},
                                                              {"bar", 1},
                                                              {"bar", 2},
                                                              {"bar", 3},
                                                              {"bar", 4},
                                                              {"bar.bar", 0},
                                                              {"bar.bar", 1},
                                                              {"bar.bar", 2},
                                                              {"baz", 3},
                                                              {"foo", 0},
                                                              {"foo", 1},
                                                              {"foo", 2},
                                                              {"foo", 4},
                                                              {"foo.bar", 0},
                                                              {"foo.bar", 2},
                                                              {"foo.bar", 3},
                                                              {"foo.bar", 4}};

    // ColumnStoreSorter uses the dbpath to store its spill files.
    ON_BLOCK_EXIT(
        [oldDbPath = storageGlobalParams.dbpath]() { storageGlobalParams.dbpath = oldDbPath; });
    unittest::TempDir tempDir("columnStoreSorterTests");
    storageGlobalParams.dbpath = tempDir.path();

    // We test two sorters: one that can perform the sort in memory and one that is constrained so
    // that it must spill to disk.

    SorterFileStats statsForInMemorySorter(nullptr);
    auto inMemorySorter = std::make_unique<ColumnStoreSorter>(
        1000000 /* maxMemoryUsageBytes */, "dbName", &statsForInMemorySorter);

    SorterFileStats statsForExternalSorter(nullptr);
    auto externalSorter = std::make_unique<ColumnStoreSorter>(
        500 /* maxMemoryUsageBytes */, "dbName", &statsForExternalSorter);

    // First, load documents into each sorter.
    for (size_t i = 0; i < sampleData.size(); ++i) {
        for (auto&& fieldName : sampleData[i]) {
            // Synthesize cell contents based on the field name and RecordId, so that we can test
            // that cell contents travel with the (Field name, RecordId) key. The null-byte
            // delimiter tests that the sorter correctly stores cells with internal null bytes.
            std::string cell = str::stream() << fieldName << "\0" << i;
            inMemorySorter->add(fieldName, RecordId(i), cell);
            externalSorter->add(fieldName, RecordId(i), cell);
        }
    }

    // Now sort, iterate the sorted output, and ensure it matches the expected output.
    std::unique_ptr<ColumnStoreSorter::Iterator> sortedItInMemory(inMemorySorter->done());
    std::unique_ptr<ColumnStoreSorter::Iterator> sortedItExternal(externalSorter->done());
    for (auto&& expected : sortedData) {
        std::string expectedCell = str::stream() << expected.first << "\0" << expected.second;

        {
            ASSERT(sortedItInMemory->more());
            auto [columnKey, columnValue] = sortedItInMemory->next();

            ASSERT_EQ(expected.first, columnKey.path);
            ASSERT_EQ(RecordId(expected.second), columnKey.recordId);
            ASSERT_EQ(expectedCell, columnValue.cell);
        }

        {
            ASSERT(sortedItExternal->more());
            auto [columnKey, columnValue] = sortedItExternal->next();

            ASSERT_EQ(expected.first, columnKey.path);
            ASSERT_EQ(RecordId(expected.second), columnKey.recordId);
            ASSERT_EQ(expectedCell, columnValue.cell);
        }
    }
    ASSERT(!sortedItInMemory->more());
    ASSERT(!sortedItExternal->more());

    sortedItInMemory.reset();
    sortedItExternal.reset();

    // Ensure that statistics for spills and file accesses are as expected.
    // Note: The number of spills in the external sorter depends on the size of C++ data structures,
    // which can be different between architectures. The test allows a range of reasonable values.
    ASSERT_EQ(0, inMemorySorter->stats().spilledRanges());
    ASSERT_LTE(3, externalSorter->stats().spilledRanges());
    ASSERT_GTE(5, externalSorter->stats().spilledRanges());

    ASSERT_EQ(0, statsForInMemorySorter.opened.load());
    ASSERT_EQ(0, statsForInMemorySorter.closed.load());

    // The external sorter has opened its spill file but will not close and delete it until it is
    // destroyed.
    ASSERT_EQ(1, statsForExternalSorter.opened.load());
    ASSERT_EQ(0, statsForExternalSorter.closed.load());

    inMemorySorter.reset();
    externalSorter.reset();

    ASSERT_EQ(0, statsForInMemorySorter.closed.load());
    ASSERT_EQ(1, statsForExternalSorter.closed.load());
}
}  // namespace mongo
