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

#pragma once

#include "mongo/db/record_id.h"
#include "mongo/db/sorter/sorter.h"
#include "mongo/db/storage/column_store.h"

namespace mongo {
/**
 * Performs the organizing and sorting steps of a column store index bulk build, presenting an
 * interface similar to the 'Sorter' interface. The client can add cells with the 'add()' method
 * until none remain and then call 'done()' to get an iterator that returns the cells in sorted
 * order.
 *
 * This class assumes that inputs are _already sorted_ by RecordId. Adding out-of-orders cells will
 * result in undefined behavior.
 *
 * Internally, this class maintains a hash table that maps each path to a sorted list of
 * (RecordId, CellValue) pairs. Because we use a hash table and not a sorted data structure (like
 * std::map), we need to sort the list of paths when finalizing the output or when writing a spill
 * file. The total number of cells inserted into this mapping is potentially very large, making it
 * preferable to defer the cost of sorting to the end in order to avoid the cost of a binary tree
 * traversal for each inserted cell.
 */
class ColumnStoreSorter : public SorterBase {
public:
    ColumnStoreSorter(size_t maxMemoryUsageBytes,
                      StringData dbName,
                      SorterFileStats* stats,
                      SorterTracker* tracker = nullptr);

    void add(PathView path, const RecordId& recordId, CellView cellContents);

    struct Key {
        PathView path;
        RecordId recordId;

        struct SorterDeserializeSettings {};

        bool operator<(const Key& other) const;
        void serializeForSorter(BufBuilder& buf) const;

        // Assumes that the source buffer will remain valid for the lifetime of the returned
        // ColumnStoreSorter::Key object.
        static Key deserializeForSorter(BufReader& buf, SorterDeserializeSettings);

        size_t memUsageForSorter() const {
            return sizeof(path) + path.size() + recordId.memUsage();
        }

        Key getOwned() const {
            MONGO_UNREACHABLE;
        }
    };

    struct Value {
        CellView cell;

        struct SorterDeserializeSettings {};

        void serializeForSorter(BufBuilder& buf) const;
        static Value deserializeForSorter(BufReader& buf, SorterDeserializeSettings);

        size_t memUsageForSorter() const {
            return sizeof(cell) + cell.size();
        }

        Value getOwned() const {
            MONGO_UNREACHABLE;
        }
    };

    using Iterator = SortIteratorInterface<Key, Value>;

    Iterator* done();

private:
    class InMemoryIterator;

    static SortOptions makeSortOptions(const std::string& dbName, SorterFileStats* stats);
    static std::string pathForNewSpillFile();

    void spill();

    Iterator* inMemoryIterator() const;

    const std::string _dbName;
    SorterFileStats* _fileStats;  // Unowned

    const size_t _maxMemoryUsageBytes;
    size_t _memUsed = 0;

    /**
     * Mapping from path name to the sorted list of (RecordId, Cell) pairs.
     */
    using CellVector = std::vector<std::pair<RecordId, CellValue>>;
    StringMap<CellVector> _dataByPath;

    std::shared_ptr<Sorter<Key, Value>::File> _spillFile;
    std::vector<std::shared_ptr<Iterator>> _spilledFileIterators;

    bool _done = false;
};
}  // namespace mongo
