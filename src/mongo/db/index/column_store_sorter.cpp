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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kIndex

#include "mongo/platform/basic.h"

#include "mongo/db/index/column_store_sorter.h"

namespace mongo {
struct ComparisonForPathAndRid {
    int operator()(const std::pair<ColumnStoreSorter::Key, ColumnStoreSorter::Value>& left,
                   const std::pair<ColumnStoreSorter::Key, ColumnStoreSorter::Value>& right) const {
        auto stringComparison = left.first.path.compare(right.first.path);
        return (stringComparison != 0) ? stringComparison
                                       : left.first.recordId.compare(right.first.recordId);
    }
};

bool ColumnStoreSorter::Key::operator<(const Key& other) const {
    if (auto cmp = path.compare(other.path); cmp != 0) {
        return cmp < 0;
    } else {
        return recordId < other.recordId;
    }
}

void ColumnStoreSorter::Key::serializeForSorter(BufBuilder& buf) const {
    buf.appendStr(path);
    recordId.serializeToken(buf);
}

ColumnStoreSorter::Key ColumnStoreSorter::Key::deserializeForSorter(
    BufReader& buf, ColumnStoreSorter::Key::SorterDeserializeSettings) {
    // Note: unlike function call parameters, the order of evaluation for initializer
    // parameters is defined.
    return {buf.readCStr(), RecordId::deserializeToken(buf)};
}

void ColumnStoreSorter::Value::serializeForSorter(BufBuilder& buf) const {
    buf.appendNum(uint32_t(cell.size()));  // Little-endian write
    buf.appendBuf(cell.rawData(), cell.size());
}

ColumnStoreSorter::Value ColumnStoreSorter::Value::deserializeForSorter(
    BufReader& buf, ColumnStoreSorter::Value::SorterDeserializeSettings) {
    size_t cellSize = buf.read<LittleEndian<uint32_t>>();
    return Value{buf.readBytes(cellSize)};
}

ColumnStoreSorter::ColumnStoreSorter(size_t maxMemoryUsageBytes,
                                     StringData dbName,
                                     SorterFileStats* stats,
                                     SorterTracker* tracker)
    : SorterBase(tracker),
      _dbName(dbName.toString()),
      _fileStats(stats),
      _maxMemoryUsageBytes(maxMemoryUsageBytes),
      _spillFile(std::make_shared<Sorter<Key, Value>::File>(pathForNewSpillFile(), _fileStats)) {}

void ColumnStoreSorter::add(PathView path, const RecordId& recordId, CellView cellContents) {
    auto& cellListAtPath = _dataByPath[path];
    if (cellListAtPath.empty()) {
        // Track memory usage of this new path.
        _memUsed += sizeof(StringMap<CellVector>::value_type) + path.size();
    }

    // The sorter assumes that RecordIds are added in sorted order.
    tassert(6548102,
            "Out-of-order record during columnar index build",
            cellListAtPath.empty() || cellListAtPath.back().first < recordId);

    cellListAtPath.emplace_back(recordId, CellValue(cellContents.rawData(), cellContents.size()));
    _memUsed += cellListAtPath.back().first.memUsage() + sizeof(CellValue) +
        cellListAtPath.back().second.size();
    if (_memUsed > _maxMemoryUsageBytes) {
        spill();
    }
}

namespace {
std::string tempDir() {
    return str::stream() << storageGlobalParams.dbpath << "/_tmp";
}
}  // namespace

SortOptions ColumnStoreSorter::makeSortOptions(const std::string& dbName, SorterFileStats* stats) {
    return SortOptions().TempDir(tempDir()).ExtSortAllowed().FileStats(stats).DBName(dbName);
}

std::string ColumnStoreSorter::pathForNewSpillFile() {
    static AtomicWord<unsigned> fileNameCounter;
    static const uint64_t randomSuffix = static_cast<uint64_t>(SecureRandom().nextInt64());
    return str::stream() << tempDir() << "/ext-sort-column-store-index."
                         << fileNameCounter.fetchAndAdd(1) << "-" << randomSuffix;
}

void ColumnStoreSorter::spill() {
    if (_dataByPath.empty()) {
        return;
    }
    this->_stats.incrementSpilledRanges();

    SortedFileWriter<Key, Value> writer(makeSortOptions(_dbName, _fileStats), _spillFile, {});

    // Cells loaded into memory are sorted by record id but not yet sorted by path. We perform that
    // sort now, so that we can output cells sorted by (path, rid) for later consumption by our
    // standard external merge implementation: SortIteratorInterface<Key, Value>::merge().
    std::vector<const StringMap<CellVector>::value_type*> sortedPathList;
    sortedPathList.reserve(_dataByPath.size());
    for (auto&& pathWithCellVector : _dataByPath) {
        sortedPathList.push_back(&pathWithCellVector);
    }
    std::sort(sortedPathList.begin(), sortedPathList.end(), [](auto left, auto right) {
        return left->first < right->first;
    });

    size_t currentChunkSize = 0;
    for (auto&& pathWithCellVector : sortedPathList) {
        auto& [path, cellVector] = *pathWithCellVector;

        size_t cellVectorSize = std::accumulate(
            cellVector.begin(), cellVector.end(), 0, [& path = path](size_t sum, auto& ridAndCell) {
                return sum + path.size() + ridAndCell.first.memUsage() + ridAndCell.second.size();
            });

        // Add (path, rid, cell) records to the spill file so that the first cell in each contiguous
        // run of cells with the same path lives in its own chunk. E.g.:
        //   Path1, rid1, Cell contents
        //   CHUNK BOUNDARY
        //   Path1, rid2, Cell Contents
        //      ...
        //   Path1, ridN, Cell Contents
        //   CHUNK BOUNDARY
        //   Path2, rid1, Cell Contents
        //   CHUNK BOUNDARY
        //   Path2, rid2, Cell Contents
        //     ...
        //
        // During merging, file readers will hold one chunk from each spill file in memory, so
        // optimizing chunk size can reduce memory usage during the merge. Merging for a column
        // store index is a special case: because the sorter is loaded in RecordId order, all the
        // cells from this spill are guaranteed to merge together, with no interleaving cells from
        // other spill files.
        //
        // This layout will result in a merger that holds a single cell from each leg of the merge
        // representing the first in a large contiguous range. Once that cell gets picked, the merge
        // will consume all chunks at that path in that file before moving on to the next file or
        // the next path.
        //
        // To avoid the pathological case where runs are very short, we don't force a chunk boundary
        // when a run of cells would not result in a chunk greater than 1024 bytes.
        const size_t kShortChunkThreshold = 1024;
        bool writeBoundaryAfterAdd = (currentChunkSize + cellVectorSize) > kShortChunkThreshold;
        if (writeBoundaryAfterAdd) {
            // Add the chunk boundary just before the first cell with this path name.
            writer.writeChunk();
            currentChunkSize = 0;
        }
        for (auto& ridAndCell : cellVector) {
            const auto& cell = ridAndCell.second;
            currentChunkSize += path.size() + ridAndCell.first.memUsage() + cell.size();
            writer.addAlreadySorted(Key{path, ridAndCell.first},
                                    Value{CellView{cell.c_str(), cell.size()}});

            if (writeBoundaryAfterAdd) {
                // Add the chunk boundary just after the first cell with this path name, giving it
                // its own chunk.
                writer.writeChunk();
                writeBoundaryAfterAdd = false;
                currentChunkSize = 0;
            }
        }
    }

    _spilledFileIterators.emplace_back(writer.done());

    _dataByPath.clear();
    _memUsed = 0;
}

ColumnStoreSorter::Iterator* ColumnStoreSorter::done() {
    invariant(!std::exchange(_done, true));

    if (_spilledFileIterators.size() == 0) {
        return inMemoryIterator();
    }

    spill();
    return SortIteratorInterface<Key, Value>::merge(
        _spilledFileIterators, makeSortOptions(_dbName, _fileStats), ComparisonForPathAndRid());
}

/**
 * This iterator "unwinds" our path -> CellVector mapping into sorted tuples of (path name,
 * recordId, cell), with the path name and recordId bundled into a single "key." The unwinding
 * proceeds using an outer iterator over the paths and an inner iterator for the current CellVector.
 * The outer iterator uses a separate path list that gets sorted when the 'InMemoryIterator' is
 * initialized. The inner iterator directly traverses the CellVector, which is already sorted.
 */
class ColumnStoreSorter::InMemoryIterator final : public ColumnStoreSorter::Iterator {
public:
    InMemoryIterator(const StringMap<CellVector>& dataByPath) {
        // Cells loaded into memory are sorted by record id but now yet by path. Sorting by path
        // finalizes the sort algorithm.
        _sortedPathList.reserve(dataByPath.size());
        for (const auto& pathWithCellVector : dataByPath) {
            _sortedPathList.push_back(&pathWithCellVector);
        }
        std::sort(_sortedPathList.begin(), _sortedPathList.end(), [](auto left, auto right) {
            return left->first < right->first;
        });

        _pathIt = _sortedPathList.begin();
        if (_pathIt != _sortedPathList.end()) {
            _cellVectorIt = (*_pathIt)->second.begin();
        }
    }

    bool more() final {
        return _pathIt != _sortedPathList.end();
    }

    std::pair<Key, Value> next() final {
        Key key{(*_pathIt)->first, _cellVectorIt->first};

        Value contents{_cellVectorIt->second};

        ++_cellVectorIt;
        while (_cellVectorIt == (*_pathIt)->second.end() && ++_pathIt != _sortedPathList.end()) {
            _cellVectorIt = (*_pathIt)->second.begin();
        }

        return {key, contents};
    }

    const std::pair<Key, Value>& current() final {
        tasserted(ErrorCodes::NotImplemented,
                  "current() not implemented for ColumnStoreSorter::Iterator");
    }

    void openSource() final {}

    void closeSource() final {}

private:
    std::vector<const StringMap<CellVector>::value_type*> _sortedPathList;

    decltype(_sortedPathList)::const_iterator _pathIt;
    CellVector::const_iterator _cellVectorIt;
};

ColumnStoreSorter::Iterator* ColumnStoreSorter::inMemoryIterator() const {
    return new InMemoryIterator(_dataByPath);
}
}  // namespace mongo

namespace {
/**
 * A 'nextFilename()' is required for the below "sorter.cpp" include to compile, but this file does
 * not use any of the 'Sorter' classes that call it.
 */
std::string nextFileName() {
    MONGO_UNREACHABLE;
}
}  // namespace

#undef MONGO_LOGV2_DEFAULT_COMPONENT
#include "mongo/db/sorter/sorter.cpp"
#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kIndex
MONGO_CREATE_SORTER(mongo::ColumnStoreSorter::Key,
                    mongo::ColumnStoreSorter::Value,
                    mongo::ComparisonForPathAndRid);
