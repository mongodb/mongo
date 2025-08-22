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

#pragma once

#include "mongo/db/sorter/sorter.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <istream>
#include <iterator>
#include <limits>
#include <memory>
#include <queue>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <snappy.h>

// IWYU pragma: no_include "boost/container/detail/std_fwd.hpp"
#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/util/spill_util.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sorter/sorter_checksum_calculator.h"
#include "mongo/db/sorter/sorter_file_name.h"
#include "mongo/db/sorter/sorter_gen.h"
#include "mongo/db/sorter/sorter_stats.h"
#include "mongo/db/stats/counters_sort.h"
#include "mongo/db/storage/encryption_hooks.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/bufreader.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/file.h"
#include "mongo/util/shared_buffer_fragment.h"
#include "mongo/util/str.h"

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

/**
 * Template definitions for Sorter implementations.
 * Separated from sorter.h because these definitions
 * are used in the definitions of Sorter callers, not
 * necessary for their public interface.
 */

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {
namespace sorter {

constexpr inline std::size_t kSortedFileBufferSize = size_t{64} << 10;

inline void checkNoExternalSortOnMongos(const SortOptions& opts) {
    // This should be checked by consumers, but if it isn't try to fail early.
    uassert(16947,
            "Attempting to use external sort from mongos. This is not allowed.",
            !(serverGlobalParams.clusterRole.hasExclusively(ClusterRole::RouterServer) &&
              opts.tempDir));
}

/**
 * Returns the current EncryptionHooks registered with the global service context.
 * Returns nullptr if the service context is not available; or if the EncyptionHooks
 * registered is not enabled.
 */
inline EncryptionHooks* getEncryptionHooksIfEnabled() {
    // Some tests may not run with a global service context.
    if (!hasGlobalServiceContext()) {
        return nullptr;
    }
    auto service = getGlobalServiceContext();
    auto encryptionHooks = EncryptionHooks::get(service);
    if (!encryptionHooks->enabled()) {
        return nullptr;
    }
    return encryptionHooks;
}

inline SharedBufferFragmentBuilder makeMemPool() {
    return SharedBufferFragmentBuilder(
        gOperationMemoryPoolBlockInitialSizeKB.loadRelaxed() * static_cast<size_t>(1024),
        SharedBufferFragmentBuilder::DoubleGrowStrategy(
            gOperationMemoryPoolBlockMaxSizeKB.loadRelaxed() * static_cast<size_t>(1024)));
}

template <typename Key, typename Comparator>
void dassertCompIsSane(const Comparator& comp, const Key& lhs, const Key& rhs) {
#if defined(MONGO_CONFIG_DEBUG_BUILD) && !defined(_MSC_VER)
    // MSVC++ already does similar verification in debug mode in addition to using
    // algorithms that do more comparisons. Doing our own verification in addition makes
    // debug builds considerably slower without any additional safety.

    // test reversed comparisons
    invariant((comp(lhs, rhs) <=> 0) == (0 <=> comp(rhs, lhs)));

    // test reflexivity
    invariant(comp(lhs, lhs) == 0);
    invariant(comp(rhs, rhs) == 0);
#endif
}

//
// Iterators
//

/**
 * Returns results from sorted in-memory storage.
 */
template <typename Key, typename Value>
class InMemIterator : public SortIteratorInterface<Key, Value> {
public:
    typedef std::pair<Key, Value> Data;

    /// No data to iterate
    InMemIterator() {}

    /// Only a single value
    InMemIterator(const Data& singleValue) : _data(1, singleValue) {}

    /// Any number of values
    template <typename Container>
    InMemIterator(const Container& input) : _data(input.begin(), input.end()) {}

    InMemIterator(std::vector<Data> data) : _data(std::move(data)) {}

    bool more() override {
        return _index < _data.size();
    }
    Data next() override {
        Data out = std::move(_data[_index]);
        _index++;
        return out;
    }

    Key nextWithDeferredValue() override {
        MONGO_UNREACHABLE;
    }

    Value getDeferredValue() override {
        MONGO_UNREACHABLE;
    }

    const Key& current() override {
        return _data[_index].first;
    }

    bool spillable() const override {
        return _index < _data.size();
    }

    std::unique_ptr<SortIteratorInterface<Key, Value>> spill(
        const SortOptions& opts, const typename Sorter<Key, Value>::Settings& settings) override {
        tassert(9917201, "spill() method is called when spillable() returns false", spillable());

        uassert(ErrorCodes::QueryExceededMemoryLimitNoDiskUseAllowed,
                "Requested to spill InMemIterator but did not opt in to external sorting",
                opts.tempDir);

        uassertStatusOK(ensureSufficientDiskSpaceForSpilling(
            *(opts.tempDir), internalQuerySpillingMinAvailableDiskSpaceBytes.load()));

        auto spillsFile =
            std::make_shared<SorterBase::File>(nextFileName(*(opts.tempDir)), opts.sorterFileStats);
        SortedFileWriter<Key, Value> writer(opts, spillsFile, settings);

        for (size_t i = _index; i < _data.size(); ++i) {
            writer.addAlreadySorted(_data[i].first, _data[i].second);
        }

        if (opts.sorterTracker) {
            opts.sorterTracker->spilledRanges.addAndFetch(1);
            opts.sorterTracker->spilledKeyValuePairs.addAndFetch(_data.size() - _index);
        }

        _data.clear();
        _data.shrink_to_fit();
        _index = 0;

        return writer.doneUnique();
    }

private:
    std::vector<Data> _data;
    uint32_t _index{0};
};

/**
 * This class is used to return the in-memory state from the sorter in read-only mode.
 * This is used by streams checkpoint use case mainly to save in-memory state on persistent
 * storage.
 */
template <typename Key, typename Value, typename Container>
class InMemReadOnlyIterator : public SortIteratorInterface<Key, Value> {
public:
    typedef std::pair<Key, Value> Data;

    InMemReadOnlyIterator(const Container& data) : _data(data) {
        _iterator = _data.begin();
    }

    bool more() override {
        return _iterator != _data.end();
    }

    Data next() override {
        Data out = *_iterator++;
        return out;
    }

    Key nextWithDeferredValue() override {
        MONGO_UNIMPLEMENTED_TASSERT(8248302);
    }

    Value getDeferredValue() override {
        MONGO_UNIMPLEMENTED_TASSERT(8248303);
    }

    const Key& current() override {
        return std::prev(_iterator)->first;
    }

private:
    const Container& _data;
    typename Container::const_iterator _iterator;
};

/**
 * Returns results from a sorted range within a file. Each instance is given a file name and start
 * and end offsets.
 */
template <typename Key, typename Value>
class FileIterator final : public SortIteratorInterface<Key, Value> {
public:
    typedef std::pair<typename Key::SorterDeserializeSettings,
                      typename Value::SorterDeserializeSettings>
        Settings;
    typedef std::pair<Key, Value> Data;

    FileIterator(std::shared_ptr<SorterBase::File> file,
                 std::streamoff fileStartOffset,
                 std::streamoff fileEndOffset,
                 const Settings& settings,
                 const boost::optional<DatabaseName>& dbName,
                 const size_t checksum,
                 const SorterChecksumVersion checksumVersion)
        : _settings(settings),
          _file(std::move(file)),
          _fileStartOffset(fileStartOffset),
          _fileCurrentOffset(fileStartOffset),
          _fileEndOffset(fileEndOffset),
          _dbName(dbName),
          _afterReadChecksumCalculator(checksumVersion),
          _originalChecksum(checksum) {}


    bool more() override {
        if (!_done)
            _fillBufferIfNeeded();  // may change _done
        return !_done;
    }

    Data next() override {
        // Note: calling read() on the _bufferReader buffer in the deserialize function advances the
        // buffer. Since Key comes before Value in the _bufferReader, and C++ makes no function
        // parameter evaluation order guarantees, we cannot deserialize Key and Value straight into
        // the Data constructor
        Key deserializedKey = nextWithDeferredValue();
        Value deserializedValue = getDeferredValue();
        return Data(std::move(deserializedKey), std::move(deserializedValue));
    }

    Key nextWithDeferredValue() override {
        invariant(!_done);
        _fillBufferIfNeeded();
        return Key::deserializeForSorter(*_bufferReader, _settings.first);
    }

    Value getDeferredValue() override {
        invariant(!_done);
        // Value is always in the same buffer as the Key, so no need to fill the buffer here
        return Value::deserializeForSorter(*_bufferReader, _settings.second);
    }

    const Key& current() override {
        tasserted(ErrorCodes::NotImplemented, "current() not implemented for FileIterator");
    }

    SorterRange getRange() const override {
        SorterRange range{
            _fileStartOffset, _fileEndOffset, static_cast<int64_t>(_originalChecksum)};
        if (_afterReadChecksumCalculator.version() != SorterChecksumVersion::v1) {
            range.setChecksumVersion(_afterReadChecksumCalculator.version());
        }
        return range;
    }

private:
    /**
     * Attempts to refill the _bufferReader if it is empty. Expects _done to be false.
     */
    void _fillBufferIfNeeded() {
        invariant(!_done);

        if (!_bufferReader || _bufferReader->atEof()) {
            _fillBufferFromDisk();
        }
        if (_done && _originalChecksum != _afterReadChecksumCalculator.checksum()) {
            fassert(31182,
                    Status(ErrorCodes::Error::ChecksumMismatch,
                           "Data read from disk does not match what was written to disk. Possible "
                           "corruption of data."));
        }
    }

    /**
     * Tries to read from disk and places any results in _bufferReader. If there is no more data to
     * read, then _done is set to true and the function returns immediately.
     */
    void _fillBufferFromDisk() {
        // The size is both written to and read from in platform-specific endian order. In the
        // unlikely event that data files are written and read by platforms of differing endianness,
        // the result will be a checksum mismatch in the worst case, which callers must recover
        // from.
        int32_t rawSize;
        _read(&rawSize, sizeof(rawSize));
        if (_done)
            return;

        // negative size means compressed
        const bool compressed = rawSize < 0;
        int32_t blockSize = std::abs(rawSize);

        _buffer = std::make_unique<char[]>(blockSize);
        _read(_buffer.get(), blockSize);
        uassert(16816, "file too short?", !_done);

        if (auto encryptionHooks = sorter::getEncryptionHooksIfEnabled()) {
            auto out = std::make_unique<char[]>(blockSize);
            DataRange outRange(out.get(), blockSize);
            Status status = encryptionHooks->unprotectTmpData(
                ConstDataRange(reinterpret_cast<const uint8_t*>(_buffer.get()), blockSize),
                &outRange,
                _dbName);
            uassert(28841,
                    str::stream() << "Failed to unprotect data: " << status.toString(),
                    status.isOK());
            blockSize = outRange.length();
            _buffer.swap(out);
        }

        if (!compressed) {
            _bufferReader = std::make_unique<BufReader>(_buffer.get(), blockSize);
            _afterReadChecksumCalculator.addData(_buffer.get(), blockSize);
            return;
        }

        dassert(snappy::IsValidCompressedBuffer(_buffer.get(), blockSize));

        size_t uncompressedSize;
        uassert(17061,
                "couldn't get uncompressed length",
                snappy::GetUncompressedLength(_buffer.get(), blockSize, &uncompressedSize));

        auto decompressionBuffer = std::make_unique<char[]>(uncompressedSize);
        uassert(17062,
                "decompression failed",
                snappy::RawUncompress(_buffer.get(), blockSize, decompressionBuffer.get()));

        // hold on to decompressed data and throw out compressed data at block exit
        _buffer.swap(decompressionBuffer);
        _bufferReader = std::make_unique<BufReader>(_buffer.get(), uncompressedSize);
        _afterReadChecksumCalculator.addData(_buffer.get(), uncompressedSize);
    }

    /**
     * Attempts to read data from disk. Sets _done to true when file offset reaches _fileEndOffset.
     */
    void _read(void* out, size_t size) {
        if (_fileCurrentOffset == _fileEndOffset) {
            _done = true;
            return;
        }

        invariant(_fileCurrentOffset < _fileEndOffset,
                  str::stream() << "Current file offset (" << _fileCurrentOffset
                                << ") greater than end offset (" << _fileEndOffset << ")");

        _file->read(_fileCurrentOffset, size, out);
        _fileCurrentOffset += size;
    }

    const Settings _settings;
    bool _done = false;

    std::unique_ptr<char[]> _buffer;
    std::unique_ptr<BufReader> _bufferReader;
    std::shared_ptr<SorterBase::File> _file;  // File containing the sorted data range.
    std::streamoff _fileStartOffset;          // File offset at which the sorted data range starts.
    std::streamoff _fileCurrentOffset;        // File offset at which we are currently reading from.
    std::streamoff _fileEndOffset;            // File offset at which the sorted data range ends.
    boost::optional<DatabaseName> _dbName;

    // Points to the beginning of a serialized key in the key-value pair currently being read, and
    // used for computing the checksum value. This is set to nullptr after reading each key-value
    // pair.
    const char* _startOfNewData = nullptr;
    // Checksum value that is updated with each read of a data object from disk. We can compare
    // this value with _originalChecksum to check for data corruption if and only if the
    // FileIterator is exhausted.
    SorterChecksumCalculator _afterReadChecksumCalculator;

    // Checksum value retrieved from SortedFileWriter that was calculated as data was spilled
    // to disk. This is not modified, and is only used for comparison against _afterReadChecksum
    // when the FileIterator is exhausted to ensure no data corruption.
    const size_t _originalChecksum;
};

/**
 * Merge-sorts results from 0 or more FileIterators, all of which should be iterating over sorted
 * ranges within the same file. The input iterators must implement nextWithDeferredValue() and
 * getDeferredValue(). This class is given the data source file name upon construction and is
 * responsible for deleting the data source file upon destruction.
 */
template <typename Key, typename Value, typename Comparator>
class MergeIterator final : public SortIteratorInterface<Key, Value> {
public:
    typedef SortIteratorInterface<Key, Value> Input;
    typedef std::pair<Key, Value> Data;

    MergeIterator(std::span<std::shared_ptr<Input>> iters,
                  const SortOptions& opts,
                  const Comparator& comp)
        : _opts(opts),
          _remaining(opts.limit ? opts.limit : std::numeric_limits<unsigned long long>::max()),
          _greater(comp) {
        for (auto& iter : iters) {
            if (iter->more()) {
                _heap.push_back(std::make_unique<Stream>(_maxFile++, iter));
            }
        }

        if (_heap.empty()) {
            _remaining = 0;
            return;
        }

        std::make_heap(_heap.begin(), _heap.end(), _greater);
        std::pop_heap(_heap.begin(), _heap.end(), _greater);
        _current = std::move(_heap.back());
        _heap.pop_back();

        _positioned = true;
    }

    ~MergeIterator() override {
        _current.reset();
        _heap.clear();
    }

    void addSource(std::shared_ptr<Input> iter) {
        if (!iter->more()) {
            return;
        }

        _heap.push_back(std::make_unique<Stream>(++_maxFile, iter));
        std::push_heap(_heap.begin(), _heap.end(), _greater);

        if (_greater(_current, _heap.front())) {
            std::pop_heap(_heap.begin(), _heap.end(), _greater);
            std::swap(_current, _heap.back());
            std::push_heap(_heap.begin(), _heap.end(), _greater);
        }
    }

    bool more() override {
        if (_remaining > 0 && (_positioned || !_heap.empty() || _current->more()))
            return true;

        _remaining = 0;
        return false;
    }

    const Key& current() override {
        invariant(_remaining);

        if (!_positioned) {
            advance();
            _positioned = true;
        }

        return _current->current();
    }

    Data next() override {
        invariant(_remaining);

        _remaining--;

        if (_positioned) {
            _positioned = false;
        } else {
            advance();
        }
        Key key = _current->current();
        Value value = _current->getDeferredValue();
        return Data(std::move(key), std::move(value));
    }

    Key nextWithDeferredValue() override {
        MONGO_UNREACHABLE;
    }

    Value getDeferredValue() override {
        MONGO_UNREACHABLE;
    }

    void advance() {
        if (!_current->advance()) {
            invariant(!_heap.empty());
            std::pop_heap(_heap.begin(), _heap.end(), _greater);
            _current = std::move(_heap.back());
            _heap.pop_back();
        } else if (!_heap.empty() && _greater(_current, _heap.front())) {
            std::pop_heap(_heap.begin(), _heap.end(), _greater);
            std::swap(_current, _heap.back());
            std::push_heap(_heap.begin(), _heap.end(), _greater);
        }
    }

private:
    /**
     * Data iterator over an Input stream.
     */
    class Stream {
    public:
        Stream(size_t fileNum, std::shared_ptr<Input> iter)
            : fileNum(fileNum), _current(iter->nextWithDeferredValue()), _rest(std::move(iter)) {}

        const Key& current() const {
            return _current;
        }
        Value getDeferredValue() {
            return _rest->getDeferredValue();
        }
        bool more() {
            return _rest->more();
        }
        bool advance() {
            if (!_rest->more())
                return false;

            _current = _rest->nextWithDeferredValue();
            return true;
        }

        const size_t fileNum;

    private:
        Key _current;
        std::shared_ptr<Input> _rest;
    };

    class STLComparator {  // uses greater rather than less-than to maintain a MinHeap
    public:
        explicit STLComparator(const Comparator& comp) : _comp(comp) {}

        template <typename Ptr>
        bool operator()(const Ptr& lhs, const Ptr& rhs) const {
            // first compare data
            dassertCompIsSane(_comp, lhs->current(), rhs->current());
            if (auto ret = _comp(lhs->current(), rhs->current()); ret != 0)
                return ret > 0;

            // then compare fileNums to ensure stability
            return lhs->fileNum > rhs->fileNum;
        }

    private:
        const Comparator _comp;
    };

    SortOptions _opts;
    unsigned long long _remaining;
    bool _positioned = false;
    std::unique_ptr<Stream> _current;
    std::vector<std::unique_ptr<Stream>> _heap;  // MinHeap
    STLComparator _greater;                      // named so calls make sense
    size_t _maxFile = 0;                         // The maximum file identifier used thus far
};

//
// Sorter types
//

template <typename Key, typename Value, typename Comparator>
class MergeableSorter : public Sorter<Key, Value> {
public:
    static constexpr std::size_t kFileIteratorSize = sizeof(FileIterator<Key, Value>);

    typedef std::pair<typename Key::SorterDeserializeSettings,
                      typename Value::SorterDeserializeSettings>
        Settings;
    typedef SortIteratorInterface<Key, Value> Iterator;

    MergeableSorter(const SortOptions& opts, const Comparator& comp, const Settings& settings)
        : Sorter<Key, Value>(opts), _comp(comp), _settings(settings) {
        setMaxMemoryUsageBytes();
    }

    MergeableSorter(const SortOptions& opts,
                    const std::string& fileName,
                    const Comparator& comp,
                    const Settings& settings)
        : Sorter<Key, Value>(opts, fileName), _comp(comp), _settings(settings) {
        setMaxMemoryUsageBytes();
    }

protected:
    /**
     * The maximum number of spills that can be merged simultaneously in order to respect memory
     * limits. While merging, a chuck of 64KB from each spill is loaded to memory. The total size of
     * chunks loaded to memory should not exceed the available memory.
     */
    size_t _spillsNumToRespectMemoryLimits =
        std::max(this->_opts.maxMemoryUsageBytes / sorter::kSortedFileBufferSize,
                 static_cast<std::size_t>(2));

    /**
     * An implementation of a k-way merge sort.
     *
     * This method will take a target number of sorted spills (numTargetedSpills) to merge and will
     * proceed to merge the set of them in batches of at most numParallelSpills until it reaches the
     * target.
     *
     * To give an example, if we have 7 spills, a target number of 2 and 3 spills can be merged in
     * parallel the algorithm will do the following:
     *
     * {1, 2, 3, 4, 5, 6, 7}
     * {123, 456, 7}
     * {1234567}
     */
    void _mergeSpills(std::size_t numTargetedSpills, std::size_t numParallelSpills) {
        using File = SorterBase::File;

        if (numTargetedSpills == 0) {
            numTargetedSpills = 1;
        }


        LOGV2_INFO(8203700,
                   "Merging spills",
                   "currentNumSpills"_attr = this->_iters.size(),
                   "targetNumSpills"_attr = numTargetedSpills,
                   "parallelNumSpills"_attr = numParallelSpills);

        std::vector<std::shared_ptr<Iterator>> iterators;
        while (this->_iters.size() > numTargetedSpills) {
            iterators.swap(this->_iters);

            std::shared_ptr<File> newSpillsFile = std::make_shared<File>(
                nextFileName(*(this->_opts.tempDir)), this->_opts.sorterFileStats);

            LOGV2_DEBUG(6033103,
                        1,
                        "Created new intermediate file for merged spills",
                        "path"_attr = newSpillsFile->path().string());

            for (std::size_t i = 0; i < iterators.size(); i += numParallelSpills) {
                auto count = std::min(numParallelSpills, iterators.size() - i);
                auto spillsToMerge = std::span(iterators).subspan(i, count);

                // Since we are merging the spills to a new file, we make sure we have sufficient
                // available disk space
                int64_t minRequiredDiskSpace = 0;
                for (auto&& it : spillsToMerge) {
                    minRequiredDiskSpace +=
                        it->getRange().getEndOffset() - it->getRange().getStartOffset();
                }
                minRequiredDiskSpace = std::max(
                    minRequiredDiskSpace,
                    static_cast<int64_t>(internalQuerySpillingMinAvailableDiskSpaceBytes.load()));
                uassertStatusOK(ensureSufficientDiskSpaceForSpilling(*(this->_opts.tempDir),
                                                                     minRequiredDiskSpace));

                LOGV2_DEBUG(6033102,
                            2,
                            "Merging spills",
                            "beginIdx"_attr = i,
                            "endIdx"_attr = i + count - 1);

                auto mergeIterator = Iterator::merge(spillsToMerge, this->_opts, _comp);
                SortedFileWriter<Key, Value> writer(this->_opts, newSpillsFile, _settings);
                uint64_t pairCount = 0;
                while (mergeIterator->more()) {
                    auto pair = mergeIterator->next();
                    writer.addAlreadySorted(pair.first, pair.second);
                    ++pairCount;
                }
                this->_iters.push_back(writer.done());
                this->_stats.incrementSpilledRanges();
                this->_stats.incrementSpilledKeyValuePairs(pairCount);
            }
            iterators.clear();
            this->_file = std::move(newSpillsFile);

            LOGV2_DEBUG(6033101,
                        1,
                        "Merged spills",
                        "currentNumSpills"_attr = this->_iters.size(),
                        "targetNumSpills"_attr = numTargetedSpills);
        }

        LOGV2_INFO(6033100, "Finished merging spills");
    }

    void _mergeSpills(std::size_t numTargetedSpills) {
        // The number of target spills and the number of parallel spills are equal.
        _mergeSpills(numTargetedSpills, numTargetedSpills);
    }

    const Comparator _comp;
    const Settings _settings;
    size_t fileIteratorsMaxBytesSize =
        1 * 1024 * 1024;  // Memory Iterators for spilled data area allowed to use.
    size_t fileIteratorsMaxNum;

private:
    // Update the maxMemoryUsageBytes subtracting the memory reserved for the file iterators. File
    // iterators can use up to maxIteratorsMemoryUsagePercentage of the maxMemoryUsageBytes with
    // lower bound fileIteratorsMaxBytesSize and upper bound 1MB.
    void setMaxMemoryUsageBytes() {
        double percRequested = maxIteratorsMemoryUsagePercentage.load();
        auto iteratorMemBytesRequested =
            static_cast<size_t>(this->_opts.maxMemoryUsageBytes * percRequested);
        if (iteratorMemBytesRequested < this->fileIteratorsMaxBytesSize) {
            this->fileIteratorsMaxBytesSize =
                std::max(kFileIteratorSize, iteratorMemBytesRequested);
        }
        this->fileIteratorsMaxNum =
            static_cast<size_t>(this->fileIteratorsMaxBytesSize / kFileIteratorSize);
        this->fileIteratorsMaxBytesSize = kFileIteratorSize * this->fileIteratorsMaxNum;

        if (this->fileIteratorsMaxBytesSize >= this->_opts.maxMemoryUsageBytes) {
            this->_opts.MaxMemoryUsageBytes(0);
        } else {
            this->_opts.MaxMemoryUsageBytes(this->_opts.maxMemoryUsageBytes -
                                            this->fileIteratorsMaxBytesSize);
        }
    }
};

template <typename Key, typename Value, typename Comparator>
class NoLimitSorter : public MergeableSorter<Key, Value, Comparator> {
public:
    typedef std::pair<Key, Value> Data;
    typedef std::function<Value()> ValueProducer;
    using Iterator = typename MergeableSorter<Key, Value, Comparator>::Iterator;
    using Settings = typename MergeableSorter<Key, Value, Comparator>::Settings;

    NoLimitSorter(const SortOptions& opts,
                  const Comparator& comp,
                  const Settings& settings = Settings())
        : MergeableSorter<Key, Value, Comparator>(opts, comp, settings) {
        invariant(opts.limit == 0);
    }

    NoLimitSorter(const std::string& fileName,
                  const std::vector<SorterRange>& ranges,
                  const SortOptions& opts,
                  const Comparator& comp,
                  const Settings& settings = Settings())
        : MergeableSorter<Key, Value, Comparator>(opts, fileName, comp, settings) {
        invariant(opts.tempDir);

        uassert(16815,
                str::stream() << "Unexpected empty file: " << this->_file->path().string(),
                ranges.empty() || boost::filesystem::file_size(this->_file->path()) != 0);

        this->_iters.reserve(ranges.size());
        std::transform(ranges.begin(),
                       ranges.end(),
                       std::back_inserter(this->_iters),
                       [this](const SorterRange& range) {
                           return std::make_shared<sorter::FileIterator<Key, Value>>(
                               this->_file,
                               range.getStartOffset(),
                               range.getEndOffset(),
                               this->_settings,
                               this->_opts.dbName,
                               range.getChecksum(),
                               range.getChecksumVersion().value_or(SorterChecksumVersion::v1));
                       });
        this->_stats.setSpilledRanges(this->_iters.size());
    }

    template <typename DataProducer>
    void addImpl(DataProducer dataProducer) {
        invariant(!_done);
        invariant(!_paused);

        auto& keyVal = _data.emplace_back(dataProducer());

        auto& memPool = this->_memPool;
        if (memPool) {
            auto memUsedInsideSorter = (sizeof(Key) + sizeof(Value)) * (_data.size() + 1);
            this->_stats.setMemUsage(memPool->memUsage() + memUsedInsideSorter);
        } else {
            auto memUsage = keyVal.first.memUsageForSorter() + keyVal.second.memUsageForSorter();
            this->_stats.incrementMemUsage(memUsage);
        }

        if (this->_stats.memUsage() > this->_opts.maxMemoryUsageBytes) {
            spill();
        }
    }

    void add(const Key& key, const Value& val) override {
        addImpl([&]() -> Data { return {key.getOwned(), val.getOwned()}; });
    }

    void emplace(Key&& key, ValueProducer valProducer) override {
        addImpl([&]() -> Data {
            key.makeOwned();
            auto val = valProducer();
            val.makeOwned();
            return {std::move(key), std::move(val)};
        });
    }

    std::unique_ptr<Iterator> done() override {
        invariant(!std::exchange(_done, true));

        if (this->_iters.empty()) {
            sort();
            if (this->_opts.moveSortedDataIntoIterator) {
                return std::make_unique<InMemIterator<Key, Value>>(std::move(_data));
            }
            return std::make_unique<InMemIterator<Key, Value>>(_data);
        }

        spill();
        this->_mergeSpills(this->_spillsNumToRespectMemoryLimits);

        return Iterator::merge(this->_iters, this->_opts, this->_comp);
    }

    std::unique_ptr<Iterator> pause() override {
        invariant(!_done);
        invariant(!_paused);

        _paused = true;
        tassert(8248300, "Spilled sort cannot be paused", this->_iters.empty());
        return std::make_unique<InMemReadOnlyIterator<Key, Value, std::vector<Data>>>(_data);
    }

    void resume() override {
        _paused = false;
    }

private:
    class STLComparator {
    public:
        explicit STLComparator(const Comparator& comp) : _comp(comp) {}
        bool operator()(const Data& lhs, const Data& rhs) const {
            dassertCompIsSane(_comp, lhs.first, rhs.first);
            return _comp(lhs.first, rhs.first) < 0;
        }

    private:
        const Comparator& _comp;
    };

    void sort() {
        STLComparator less(this->_comp);
        std::sort(_data.begin(), _data.end(), less);
        this->_stats.incrementNumSorted(_data.size());
        auto& memPool = this->_memPool;
        if (memPool) {
            invariant(memPool->totalFragmentBytesUsed() >= this->_stats.bytesSorted());
            this->_stats.incrementBytesSorted(memPool->totalFragmentBytesUsed() -
                                              this->_stats.bytesSorted());
        } else {
            this->_stats.incrementBytesSorted(this->_stats.memUsage());
        }
    }

    void spill() override {
        if (_data.empty()) {
            return;
        }

        if (!this->_opts.tempDir) {
            // This error message only applies to sorts from user queries made through the find or
            // aggregation commands. Other clients, such as bulk index builds, should suppress this
            // error, either by allowing external sorting or by catching and throwing a more
            // appropriate error.
            uasserted(ErrorCodes::QueryExceededMemoryLimitNoDiskUseAllowed,
                      str::stream()
                          << "Sort exceeded memory limit of "
                          << (this->_opts.maxMemoryUsageBytes + this->fileIteratorsMaxBytesSize)
                          << " bytes, but did not opt in to external sorting.");
        }

        // Ensure there is sufficient disk space for spilling
        uassertStatusOK(ensureSufficientDiskSpaceForSpilling(
            *(this->_opts.tempDir), internalQuerySpillingMinAvailableDiskSpaceBytes.load()));

        sort();

        SortedFileWriter<Key, Value> writer(this->_opts, this->_file, this->_settings);
        for (auto& data : _data) {
            writer.addAlreadySorted(data.first, data.second);
        }

        this->_stats.incrementSpilledKeyValuePairs(_data.size());
        _data.clear();
        // _data may have grown very large. Even though it's clear()ed, we need to
        // free the excess memory.
        _data.shrink_to_fit();
        this->_iters.push_back(writer.done());

        auto& memPool = this->_memPool;
        if (memPool) {
            // We expect that all buffers are unused at this point.
            memPool->freeUnused();
            this->_stats.setMemUsage(memPool->memUsage());
        } else {
            this->_stats.resetMemUsage();
        }

        this->_stats.incrementSpilledRanges();

        // Merge spills to remain below the `fileIteratorsMaxBytesSize` threshold.
        if (this->_iters.size() >= this->fileIteratorsMaxNum) {
            this->_mergeSpills(this->_iters.size() / 2, this->_spillsNumToRespectMemoryLimits);
        }
    }

    std::vector<Data> _data;  // Data that has not been spilled.
    bool _done = false;
    bool _paused = false;
};

template <typename Key, typename Value, typename Comparator>
class LimitOneSorter : public Sorter<Key, Value> {
    // Since this class is only used for limit==1, it omits all logic to
    // spill to disk and only tracks memory usage if explicitly requested.
public:
    typedef std::pair<Key, Value> Data;
    typedef std::function<Value()> ValueProducer;
    typedef SortIteratorInterface<Key, Value> Iterator;

    LimitOneSorter(const SortOptions& opts, const Comparator& comp)
        : Sorter<Key, Value>(opts), _comp(comp), _haveData(false) {
        invariant(opts.limit == 1);
    }

    template <typename DataProducer>
    void addImpl(const Key& key, DataProducer dataProducer) {
        this->_stats.incrementNumSorted();
        if (_haveData) {
            dassertCompIsSane(_comp, _best.first, key);
            if (_comp(_best.first, key) <= 0)
                return;  // not good enough
        } else {
            _haveData = true;
        }

        // Invoking dataProducer could invalidate key if it uses move semantics,
        // don't reference them anymore from this point on.
        _best = dataProducer();
    }

    void add(const Key& key, const Value& val) override {
        addImpl(key, [&]() -> Data { return {key.getOwned(), val.getOwned()}; });
    }

    void emplace(Key&& key, ValueProducer valProducer) override {
        addImpl(key, [&]() -> Data {
            key.makeOwned();
            auto val = valProducer();
            val.makeOwned();
            return {std::move(key), std::move(val)};
        });
    }

    std::unique_ptr<Iterator> done() override {
        if (_haveData) {
            if (this->_opts.moveSortedDataIntoIterator) {
                return std::make_unique<InMemIterator<Key, Value>>(std::move(_best));
            }
            return std::make_unique<InMemIterator<Key, Value>>(_best);
        } else {
            return std::make_unique<InMemIterator<Key, Value>>();
        }
    }

    std::unique_ptr<Iterator> pause() override {
        if (_haveData) {
            // ok to return InMemIterator as this is a single value constructed from copy
            return std::make_unique<InMemIterator<Key, Value>>(_best);
        } else {
            return std::make_unique<InMemIterator<Key, Value>>();
        }
    }

    void resume() override {}

private:
    void spill() override {
        invariant(false, "LimitOneSorter does not spill to disk");
    }

    const Comparator _comp;
    Data _best;
    bool _haveData;  // false at start, set to true on first call to add()
};

template <typename Key, typename Value, typename Comparator>
class TopKSorter : public MergeableSorter<Key, Value, Comparator> {
public:
    typedef std::pair<Key, Value> Data;
    typedef std::function<Value()> ValueProducer;
    using Iterator = typename MergeableSorter<Key, Value, Comparator>::Iterator;
    using Settings = typename MergeableSorter<Key, Value, Comparator>::Settings;

    TopKSorter(const SortOptions& opts,
               const Comparator& comp,
               const Settings& settings = Settings())
        : MergeableSorter<Key, Value, Comparator>(opts, comp, settings),
          _haveCutoff(false),
          _worstCount(0),
          _medianCount(0) {
        // This also *works* with limit==1 but LimitOneSorter should be used instead
        invariant(opts.limit > 1);

        // Preallocate a fixed sized vector of the required size if we don't expect it to have a
        // major impact on our memory budget. This is the common case with small limits.
        if (opts.limit <
            std::min((opts.maxMemoryUsageBytes / 10) / sizeof(typename decltype(_data)::value_type),
                     _data.max_size())) {
            _data.reserve(opts.limit);
        }
    }

    template <typename DataProducer>
    void addImpl(const Key& key, DataProducer dataProducer) {
        invariant(!_done);
        invariant(!_paused);

        this->_stats.incrementNumSorted();

        STLComparator less(this->_comp);

        if (_data.size() < this->_opts.limit) {
            if (_haveCutoff && this->_comp(key, _cutoff.first) >= 0)
                return;

            // Invoking dataProducer could invalidate key if it uses move semantics,
            // don't reference them anymore from this point on.
            auto& keyVal = _data.emplace_back(dataProducer());

            auto memUsage = keyVal.first.memUsageForSorter() + keyVal.second.memUsageForSorter();
            this->_stats.incrementMemUsage(memUsage);

            if (_data.size() == this->_opts.limit)
                std::make_heap(_data.begin(), _data.end(), less);

            if (this->_stats.memUsage() > this->_opts.maxMemoryUsageBytes)
                spill();

            return;
        }

        invariant(_data.size() == this->_opts.limit);

        if (this->_comp(key, _data.front().first) >= 0)
            return;  // not good enough

        // Remove the old worst pair and insert the contender, adjusting _memUsed

        this->_stats.decrementMemUsage(_data.front().first.memUsageForSorter());
        this->_stats.decrementMemUsage(_data.front().second.memUsageForSorter());

        std::pop_heap(_data.begin(), _data.end(), less);

        // Invoking dataProducer could invalidate key if it uses move semantics,
        // don't reference them anymore from this point on.
        _data.back() = dataProducer();

        this->_stats.incrementMemUsage(_data.back().first.memUsageForSorter());
        this->_stats.incrementMemUsage(_data.back().second.memUsageForSorter());

        std::push_heap(_data.begin(), _data.end(), less);

        if (this->_stats.memUsage() > this->_opts.maxMemoryUsageBytes)
            spill();
    }

    void add(const Key& key, const Value& val) override {
        addImpl(key, [&]() -> Data { return {key.getOwned(), val.getOwned()}; });
    }

    void emplace(Key&& key, ValueProducer valProducer) override {
        addImpl(key, [&]() -> Data {
            key.makeOwned();
            auto val = valProducer();
            val.makeOwned();
            return {std::move(key), std::move(val)};
        });
    }

    std::unique_ptr<Iterator> done() override {
        if (this->_iters.empty()) {
            sort();
            if (this->_opts.moveSortedDataIntoIterator) {
                return std::make_unique<InMemIterator<Key, Value>>(std::move(_data));
            }
            return std::make_unique<InMemIterator<Key, Value>>(_data);
        }

        spill();
        this->_mergeSpills(this->_spillsNumToRespectMemoryLimits);

        _done = true;
        return Iterator::merge(this->_iters, this->_opts, this->_comp);
    }

    std::unique_ptr<Iterator> pause() override {
        invariant(!_done);
        invariant(!_paused);
        _paused = true;

        tassert(8248301, "Spilled sort cannot be paused", this->_iters.empty());
        return std::make_unique<InMemReadOnlyIterator<Key, Value, std::vector<Data>>>(_data);
    }

    void resume() override {
        _paused = false;
    }

private:
    class STLComparator {
    public:
        explicit STLComparator(const Comparator& comp) : _comp(comp) {}
        bool operator()(const Data& lhs, const Data& rhs) const {
            dassertCompIsSane(_comp, lhs.first, rhs.first);
            return _comp(lhs.first, rhs.first) < 0;
        }

    private:
        const Comparator& _comp;
    };

    void sort() {
        STLComparator less(this->_comp);

        if (_data.size() == this->_opts.limit) {
            std::sort_heap(_data.begin(), _data.end(), less);
        } else {
            std::sort(_data.begin(), _data.end(), less);
        }

        this->_stats.incrementBytesSorted(this->_stats.memUsage());
    }

    // Can only be called after _data is sorted
    void updateCutoff() {
        // Theory of operation: We want to be able to eagerly ignore values we know will not
        // be in the TopK result set by setting _cutoff to a value we know we have at least
        // K values equal to or better than. There are two values that we track to
        // potentially become the next value of _cutoff: _worstSeen and _lastMedian. When
        // one of these values becomes the new _cutoff, its associated counter is reset to 0
        // and a new value is chosen for that member the next time we spill.
        //
        // _worstSeen is the worst value we've seen so that all kept values are better than
        // (or equal to) it. This means that once _worstCount >= _opts.limit there is no
        // reason to consider values worse than _worstSeen so it can become the new _cutoff.
        // This technique is especially useful when the input is already roughly sorted (eg
        // sorting ASC on an ObjectId or Date field) since we will quickly find a cutoff
        // that will exclude most later values, making the full TopK operation including
        // the MergeIterator phase is O(K) in space and O(N + K*Log(K)) in time.
        //
        // _lastMedian was the median of the _data in the first spill() either overall or
        // following a promotion of _lastMedian to _cutoff. We count the number of kept
        // values that are better than or equal to _lastMedian in _medianCount and can
        // promote _lastMedian to _cutoff once _medianCount >=_opts.limit. Assuming
        // reasonable median selection (which should happen when the data is completely
        // unsorted), after the first K spilled values, we will keep roughly 50% of the
        // incoming values, 25% after the second K, 12.5% after the third K, etc. This means
        // that by the time we spill 3*K values, we will have seen (1*K + 2*K + 4*K) values,
        // so the expected number of kept values is O(Log(N/K) * K). The final run time if
        // using the O(K*Log(N)) merge algorithm in MergeIterator is O(N + K*Log(K) +
        // K*LogLog(N/K)) which is much closer to O(N) than O(N*Log(K)).
        //
        // This leaves a currently unoptimized worst case of data that is already roughly
        // sorted, but in the wrong direction, such that the desired results are all the
        // last ones seen. It will require O(N) space and O(N*Log(K)) time. Since this
        // should be trivially detectable, as a future optimization it might be nice to
        // detect this case and reverse the direction of input (if possible) which would
        // turn this into the best case described above.
        //
        // Pedantic notes: The time complexities above (which count number of comparisons)
        // ignore the sorting of batches prior to spilling to disk since they make it more
        // confusing without changing the results. If you want to add them back in, add an
        // extra term to each time complexity of (SPACE_COMPLEXITY * Log(BATCH_SIZE)). Also,
        // all space complexities measure disk space rather than memory since this class is
        // O(1) in memory due to the _opts.maxMemoryUsageBytes limit.

        STLComparator less(this->_comp);  // less is "better" for TopK.

        // Pick a new _worstSeen or _lastMedian if should.
        if (_worstCount == 0 || less(_worstSeen, _data.back())) {
            _worstSeen = _data.back();
        }
        if (_medianCount == 0) {
            size_t medianIndex = _data.size() / 2;  // chooses the higher if size() is even.
            _lastMedian = _data[medianIndex];
        }

        // Add the counters of kept objects better than or equal to _worstSeen/_lastMedian.
        _worstCount += _data.size();  // everything is better or equal
        typename std::vector<Data>::iterator firstWorseThanLastMedian =
            std::upper_bound(_data.begin(), _data.end(), _lastMedian, less);
        _medianCount += std::distance(_data.begin(), firstWorseThanLastMedian);


        // Promote _worstSeen or _lastMedian to _cutoff and reset counters if should.
        if (_worstCount >= this->_opts.limit) {
            if (!_haveCutoff || less(_worstSeen, _cutoff)) {
                _cutoff = _worstSeen;
                _haveCutoff = true;
            }
            _worstCount = 0;
        }
        if (_medianCount >= this->_opts.limit) {
            if (!_haveCutoff || less(_lastMedian, _cutoff)) {
                _cutoff = _lastMedian;
                _haveCutoff = true;
            }
            _medianCount = 0;
        }
    }

    void spill() override {
        if (_data.empty())
            return;

        invariant(!_done);

        if (!this->_opts.tempDir) {
            // This error message only applies to sorts from user queries made through the find or
            // aggregation commands. Other clients should suppress this error, either by allowing
            // external sorting or by catching and throwing a more appropriate error.
            uasserted(ErrorCodes::QueryExceededMemoryLimitNoDiskUseAllowed,
                      str::stream()
                          << "Sort exceeded memory limit of "
                          << (this->_opts.maxMemoryUsageBytes + this->fileIteratorsMaxBytesSize)
                          << " bytes, but did not opt in to external sorting. Aborting operation."
                          << " Pass allowDiskUse:true to opt in.");
        }

        sort();
        updateCutoff();

        SortedFileWriter<Key, Value> writer(this->_opts, this->_file, this->_settings);
        for (size_t i = 0; i < _data.size(); i++) {
            writer.addAlreadySorted(_data[i].first, _data[i].second);
        }

        this->_stats.incrementSpilledKeyValuePairs(_data.size());
        _data.clear();
        // _data may have grown very large. Even though it's clear()ed, we need to
        // free the excess memory.
        _data.shrink_to_fit();

        this->_iters.push_back(writer.done());

        this->_stats.resetMemUsage();
        this->_stats.incrementSpilledRanges();

        // Merge spills to remain below the `fileIteratorsMaxBytesSize` threshold.
        if (this->_iters.size() >= this->fileIteratorsMaxNum) {
            this->_mergeSpills(this->_iters.size() / 2, this->_spillsNumToRespectMemoryLimits);
        }
    }

    bool _done = false;
    bool _paused = false;

    // Data that has not been spilled. Organized as max-heap if size == limit.
    std::vector<Data> _data;

    // See updateCutoff() for a full description of how these members are used.
    bool _haveCutoff;
    Data _cutoff;         // We can definitely ignore values worse than this.
    Data _worstSeen;      // The worst Data seen so far. Reset when _worstCount >= _opts.limit.
    size_t _worstCount;   // Number of docs better or equal to _worstSeen kept so far.
    Data _lastMedian;     // Median of a batch. Reset when _medianCount >= _opts.limit.
    size_t _medianCount;  // Number of docs better or equal to _lastMedian kept so far.
};

}  // namespace sorter

//
// Sorter members
//

template <typename Key, typename Value>
Sorter<Key, Value>::Sorter(const SortOptions& opts)
    : SorterBase(opts.sorterTracker),
      _opts(opts),
      _file(opts.tempDir ? std::make_shared<SorterBase::File>(sorter::nextFileName(*(opts.tempDir)),
                                                              opts.sorterFileStats)
                         : nullptr) {
    if (opts.useMemPool) {
        _memPool.emplace(sorter::makeMemPool());
    }
}

template <typename Key, typename Value>
Sorter<Key, Value>::Sorter(const SortOptions& opts, const std::string& fileName)
    : SorterBase(opts.sorterTracker),
      _opts(opts),
      _file(std::make_shared<SorterBase::File>(*(opts.tempDir) + "/" + fileName,
                                               opts.sorterFileStats)) {
    invariant(opts.tempDir);
    invariant(!fileName.empty());
    if (opts.useMemPool) {
        _memPool.emplace(sorter::makeMemPool());
    }
}

template <typename Key, typename Value>
typename Sorter<Key, Value>::PersistedState Sorter<Key, Value>::persistDataForShutdown() {
    spill();
    this->_file->keep();

    std::vector<SorterRange> ranges;
    ranges.reserve(_iters.size());
    std::transform(_iters.begin(), _iters.end(), std::back_inserter(ranges), [](auto&& it) {
        return it->getRange();
    });

    return {_file->path().filename().string(), ranges};
}

//
// SorterBase::File members
//

inline SorterBase::File::File(boost::filesystem::path path, SorterFileStats* stats)
    : _path(std::move(path)), _stats(stats) {
    invariant(!_path.empty());
    if (_stats && boost::filesystem::exists(_path) && boost::filesystem::is_regular_file(_path)) {
        _stats->addSpilledDataSize(boost::filesystem::file_size(_path));
    }
}

inline SorterBase::File::~File() {
    if (_stats && _file.is_open()) {
        _stats->closed.addAndFetch(1);
    }

    if (_keep) {
        if (!_file.is_open()) {
            return;
        }
        try {
            _file.flush();
        } catch (...) {
            reportFailedDestructor(MONGO_SOURCE_LOCATION());
        }

        mongo::File fileForFsync;
        fileForFsync.open(_path.string().c_str());
        if (fileForFsync.is_open()) {
            fileForFsync.fsync();
        }

        return;
    }

    if (_file.is_open()) {
        try {
            _file.exceptions(std::ios::failbit);
        } catch (...) {
            reportFailedDestructor(MONGO_SOURCE_LOCATION());
        }
        try {
            _file.close();
        } catch (...) {
            reportFailedDestructor(MONGO_SOURCE_LOCATION());
        }
    }

    try {
        boost::filesystem::remove(_path);
    } catch (...) {
        reportFailedDestructor(MONGO_SOURCE_LOCATION());
    }
}

inline void SorterBase::File::read(std::streamoff offset, std::streamsize size, void* out) {
    if (!_file.is_open()) {
        _open();
    }

    // If the _offset is not -1, we may have written data to it, so we must flush.
    if (_offset != -1) {
        _file.exceptions(std::ios::goodbit);
        _file.flush();
        _offset = -1;

        uassert(5479100,
                str::stream() << "Error flushing file " << _path.string() << ": "
                              << errorMessage(lastPosixError()),
                _file);
    }

    _file.seekg(offset);
    _file.read(reinterpret_cast<char*>(out), size);

    uassert(16817,
            str::stream() << "Error reading file " << _path.string() << ": "
                          << errorMessage(lastPosixError()),
            _file);

    invariant(_file.gcount() == size,
              str::stream() << "Number of bytes read (" << _file.gcount()
                            << ") not equal to expected number (" << size << ")");

    uassert(51049,
            str::stream() << "Error reading file " << _path.string() << ": "
                          << errorMessage(lastPosixError()),
            _file.tellg() >= 0);
}

inline void SorterBase::File::write(const char* data, std::streamsize size) {
    _ensureOpenForWriting();

    try {
        _file.write(data, size);
        _offset += size;
        if (_stats) {
            this->_stats->addSpilledDataSize(size);
        };
    } catch (const std::system_error& ex) {
        if (ex.code() == std::errc::no_space_on_device) {
            uasserted(ErrorCodes::OutOfDiskSpace,
                      str::stream() << ex.what() << ": " << _path.string());
        }
        uasserted(5642403,
                  str::stream() << "Error writing to file " << _path.string() << ": "
                                << errorMessage(lastPosixError()));
    } catch (const std::exception&) {
        uasserted(16821,
                  str::stream() << "Error writing to file " << _path.string() << ": "
                                << errorMessage(lastPosixError()));
    }
}

inline std::streamoff SorterBase::File::currentOffset() {
    _ensureOpenForWriting();
    invariant(_offset >= 0);
    return _offset;
}

inline void SorterBase::File::_open() {
    invariant(!_file.is_open());

    boost::filesystem::create_directories(_path.parent_path());

    // We open the provided file in append mode so that SortedFileWriter instances can share
    // the same file, used serially. We want to share files in order to stay below system
    // open file limits.
    _file.open(_path.string(), std::ios::app | std::ios::binary | std::ios::in | std::ios::out);

    uassert(16818,
            str::stream() << "Error opening file " << _path.string() << ": "
                          << errorMessage(lastPosixError()),
            _file.good());

    if (_stats) {
        _stats->opened.addAndFetch(1);
    }
}

inline void SorterBase::File::_ensureOpenForWriting() {
    if (!_file.is_open()) {
        _open();
    }

    // If we are opening the file for the first time, or if we previously flushed and switched to
    // read mode, we need to set the _offset to the file size.
    if (_offset == -1) {
        _file.exceptions(std::ios::failbit | std::ios::badbit);
        _offset = boost::filesystem::file_size(_path);
        _file.seekp(_offset);
    }
}

//
// SortedStorageWriter
//
template <typename Key, typename Value>
SortedStorageWriter<Key, Value>::SortedStorageWriter(const SortOptions& opts,
                                                     const Settings& settings)
    : _settings(settings), _checksumCalculator(opts.checksumVersion), _opts(opts) {
    // This should be checked by consumers, but if we get here don't allow writes.
    uassert(16946,
            "Attempting to use external sort from mongos. This is not allowed.",
            !serverGlobalParams.clusterRole.hasExclusively(ClusterRole::RouterServer));
}

//
// SortedFileWriter
//

template <typename Key, typename Value>
SortedFileWriter<Key, Value>::SortedFileWriter(const SortOptions& opts,
                                               std::shared_ptr<SorterBase::File> file,
                                               const Settings& settings)
    : SortedStorageWriter<Key, Value>(opts, settings),
      _file(std::move(file)),
      _fileStartOffset(_file->currentOffset()) {}

template <typename Key, typename Value>
void SortedFileWriter<Key, Value>::addAlreadySorted(const Key& key, const Value& val) {
    // Add serialized key and value to the buffer.
    key.serializeForSorter(this->_buffer);
    val.serializeForSorter(this->_buffer);

    if (this->_buffer.len() > static_cast<int>(sorter::kSortedFileBufferSize))
        writeChunk();
}

template <typename Key, typename Value>
void SortedFileWriter<Key, Value>::writeChunk() {
    int32_t size = this->_buffer.len();
    char* outBuffer = this->_buffer.buf();

    if (size == 0)
        return;

    this->_checksumCalculator.addData(outBuffer, size);

    if (this->_opts.sorterFileStats) {
        this->_opts.sorterFileStats->addSpilledDataSizeUncompressed(size);
    }

    std::string compressed;
    snappy::Compress(outBuffer, size, &compressed);
    invariant(compressed.size() <= size_t(std::numeric_limits<int32_t>::max()));

    const bool shouldCompress = compressed.size() < (size_t(this->_buffer.len()) / 10 * 9);
    if (shouldCompress) {
        size = compressed.size();
        outBuffer = const_cast<char*>(compressed.data());
    }

    std::unique_ptr<char[]> out;
    if (auto encryptionHooks = sorter::getEncryptionHooksIfEnabled()) {
        size_t protectedSizeMax = size + encryptionHooks->additionalBytesForProtectedBuffer();
        out = std::make_unique<char[]>(protectedSizeMax);
        DataRange outRange(out.get(), protectedSizeMax);
        Status status = encryptionHooks->protectTmpData(
            ConstDataRange(reinterpret_cast<const uint8_t*>(outBuffer), size),
            &outRange,
            this->_opts.dbName);
        uassert(28842,
                str::stream() << "Failed to compress data: " << status.toString(),
                status.isOK());
        outBuffer = out.get();
        size = outRange.length();
    }

    // Negative size means compressed.
    int32_t signedSize = shouldCompress ? -size : size;

    // The size is both written to and read from in platform-specific endian order. In the unlikely
    // event that data files are written and read by platforms of differing endianness, the result
    // will be a read checksum mismatch in the worst case, which callers must recover from.
    _file->write(reinterpret_cast<const char*>(&signedSize), sizeof(signedSize));
    _file->write(outBuffer, size);
    sortCounters.incrementSortCountersPerSpilling(/*sortSpills=*/1, sizeof(signedSize) + size);

    this->_buffer.reset();
}

template <typename Key, typename Value>
std::shared_ptr<SortIteratorInterface<Key, Value>> SortedFileWriter<Key, Value>::done() {
    writeChunk();

    return std::make_shared<sorter::FileIterator<Key, Value>>(_file,
                                                              _fileStartOffset,
                                                              _file->currentOffset(),
                                                              this->_settings,
                                                              this->_opts.dbName,
                                                              this->_checksumCalculator.checksum(),
                                                              this->_checksumCalculator.version());
}

template <typename Key, typename Value>
std::unique_ptr<SortIteratorInterface<Key, Value>> SortedFileWriter<Key, Value>::doneUnique() {
    writeChunk();

    return std::make_unique<sorter::FileIterator<Key, Value>>(_file,
                                                              _fileStartOffset,
                                                              _file->currentOffset(),
                                                              this->_settings,
                                                              this->_opts.dbName,
                                                              this->_checksumCalculator.checksum(),
                                                              this->_checksumCalculator.version());
}

//
// BoundedSorter members
//

template <typename Key, typename Value, typename Comparator, typename BoundMaker>
BoundedSorter<Key, Value, Comparator, BoundMaker>::BoundedSorter(const SortOptions& opts,
                                                                 Comparator comp,
                                                                 BoundMaker makeBound,
                                                                 bool checkInput)
    : BoundedSorterInterface<Key, Value>(opts),
      compare(comp),
      makeBound(makeBound),
      _checkInput(checkInput),
      _opts(opts),
      _heap(Greater{&compare}),
      _file(opts.tempDir ? std::make_shared<typename SorterBase::File>(
                               sorter::nextFileName(*(opts.tempDir)), opts.sorterFileStats)
                         : nullptr) {}

template <typename Key, typename Value, typename Comparator, typename BoundMaker>
void BoundedSorter<Key, Value, Comparator, BoundMaker>::add(Key key, Value value) {
    invariant(!_done);
    // If a new value violates what we thought was our min bound, something has gone wrong.
    uassert(6369910,
            str::stream() << "BoundedSorter input is too out-of-order: with bound "
                          << _min->toString() << ", did not expect input " << key.toString(),
            !_checkInput || !_min || compare(*_min, key) <= 0);

    // Each new item can potentially give us a tighter bound (a higher min).
    setBound(makeBound(key, value));

    auto memUsage = key.memUsageForSorter() + value.memUsageForSorter();
    _heap.emplace(std::move(key), std::move(value));

    this->_stats.incrementMemUsage(memUsage);
    this->_stats.incrementBytesSorted(memUsage);
    if (this->_stats.memUsage() > _opts.maxMemoryUsageBytes)
        _spill(_opts.maxMemoryUsageBytes);
}

template <typename Key, typename Value, typename Comparator, typename BoundMaker>
void BoundedSorter<Key, Value, Comparator, BoundMaker>::restart() {
    tassert(
        6434804, "BoundedSorter must be in state kDone to restart()", getState() == State::kDone);

    // In state kDone, the heap and spill are usually empty, because kDone means the sorter has
    // no more elements to return. However, if there is a limit then we can also reach state
    // kDone when 'this->_stats.numSorted() == _opts.limit'.
    _spillIter.reset();
    _heap = decltype(_heap){Greater{&compare}};
    this->_stats.resetMemUsage();

    _done = false;
    _min.reset();

    // There are now two possible states we could be in:
    // - Typically, we should be ready for more input (kWait).
    // - If there is a limit and we reached it, then we're done. We were done before restart()
    //   and we're still done.
    if (_opts.limit && this->_stats.numSorted() == _opts.limit) {
        tassert(6434806,
                "BoundedSorter has fulfilled _opts.limit and should still be in state kDone",
                getState() == State::kDone);
    } else {
        tassert(6434805, "BoundedSorter should now be ready for input", getState() == State::kWait);
    }
}

template <typename Key, typename Value, typename Comparator, typename BoundMaker>
typename BoundedSorterInterface<Key, Value>::State
BoundedSorter<Key, Value, Comparator, BoundMaker>::getState() const {
    if (_opts.limit > 0 && _opts.limit == this->_stats.numSorted()) {
        return State::kDone;
    }

    if (_done) {
        // No more input will arrive, so we're never in state kWait.
        return _heap.empty() && !_spillIter ? State::kDone : State::kReady;
    }

    if (_heap.empty() && !_spillIter)
        return State::kWait;

    // _heap.top() is the min of _heap, but we also need to consider whether a smaller input
    // will arrive later. So _heap.top() is safe to return only if _heap.top() < _min.
    if (!_heap.empty() && compare(_heap.top().first, *_min) < 0)
        return State::kReady;

    // Similarly, we can return the next element from the spilled iterator if it's < _min.
    if (_spillIter && compare(_spillIter->current(), *_min) < 0)
        return State::kReady;

    // A later call to add() may improve _min. Or in the worst case, after done() is called
    // we will return everything in _heap.
    return State::kWait;
}

template <typename Key, typename Value, typename Comparator, typename BoundMaker>
std::pair<Key, Value> BoundedSorter<Key, Value, Comparator, BoundMaker>::next() {
    dassert(getState() == State::kReady);
    std::pair<Key, Value> result;

    auto pullFromHeap = [this, &result]() {
        result = std::move(_heap.top());
        _heap.pop();

        auto memUsage = result.first.memUsageForSorter() + result.second.memUsageForSorter();
        this->_stats.decrementMemUsage(memUsage);
    };

    auto pullFromSpilled = [this, &result]() {
        result = _spillIter->next();
        if (!_spillIter->more()) {
            _spillIter.reset();
        }
    };

    if (!_heap.empty() && _spillIter) {
        if (compare(_heap.top().first, _spillIter->current()) <= 0) {
            pullFromHeap();
        } else {
            pullFromSpilled();
        }
    } else if (!_heap.empty()) {
        pullFromHeap();
    } else {
        pullFromSpilled();
    }

    this->_stats.incrementNumSorted();

    return result;
}

template <typename Key, typename Value, typename Comparator, typename BoundMaker>
void BoundedSorter<Key, Value, Comparator, BoundMaker>::setBound(Key key) {
    if (!_min || compare(*_min, key) < 0) {
        _min = key;
    }
}

template <typename Key, typename Value, typename Comparator, typename BoundMaker>
void BoundedSorter<Key, Value, Comparator, BoundMaker>::_spill(size_t maxMemoryUsageBytes) {
    if (_heap.empty())
        return;

    // If we have a small $limit, we can simply extract that many of the smallest elements from
    // the _heap and discard the rest, avoiding an expensive spill to disk.
    if (_opts.limit > 0 && _opts.limit < (_heap.size() / 2)) {
        this->_stats.resetMemUsage();
        decltype(_heap) retained{Greater{&compare}};
        for (size_t i = 0; i < _opts.limit; ++i) {
            this->_stats.incrementMemUsage(_heap.top().first.memUsageForSorter() +
                                           _heap.top().second.memUsageForSorter());
            retained.emplace(_heap.top());
            _heap.pop();
        }
        _heap.swap(retained);

        if (this->_stats.memUsage() < maxMemoryUsageBytes) {
            return;
        }
    }

    uassert(ErrorCodes::QueryExceededMemoryLimitNoDiskUseAllowed,
            str::stream() << "Sort exceeded memory limit of " << maxMemoryUsageBytes
                          << " bytes, but did not opt in to external sorting.",
            _opts.tempDir);

    uassertStatusOK(ensureSufficientDiskSpaceForSpilling(
        *(_opts.tempDir), internalQuerySpillingMinAvailableDiskSpaceBytes.loadRelaxed()));

    this->_stats.incrementSpilledKeyValuePairs(_heap.size());
    this->_stats.incrementSpilledRanges();

    // Write out all the values from the heap in sorted order.
    SortedFileWriter<Key, Value> writer(_opts, _file, {});
    while (!_heap.empty()) {
        writer.addAlreadySorted(_heap.top().first, _heap.top().second);
        _heap.pop();
    }
    auto iteratorPtr = writer.done();

    if (auto* mergeIter = static_cast<typename sorter::MergeIterator<Key, Value, Comparator>*>(
            _spillIter.get())) {
        mergeIter->addSource(std::move(iteratorPtr));
    } else {
        _spillIter = SpillIterator::merge(std::span(&iteratorPtr, 1), _opts, compare);
    }

    dassert(_spillIter->more());

    this->_stats.resetMemUsage();
}

//
// Factory Functions
//

template <typename Key, typename Value>
template <typename Comparator>
std::unique_ptr<SortIteratorInterface<Key, Value>> SortIteratorInterface<Key, Value>::merge(
    std::span<std::shared_ptr<SortIteratorInterface>> iters,
    const SortOptions& opts,
    const Comparator& comp) {
    return std::make_unique<sorter::MergeIterator<Key, Value, Comparator>>(iters, opts, comp);
}

template <typename Key, typename Value>
template <typename Comparator>
std::unique_ptr<Sorter<Key, Value>> Sorter<Key, Value>::make(const SortOptions& opts,
                                                             const Comparator& comp,
                                                             const Settings& settings) {
    sorter::checkNoExternalSortOnMongos(opts);
    switch (opts.limit) {
        case 0:
            return std::make_unique<sorter::NoLimitSorter<Key, Value, Comparator>>(
                opts, comp, settings);
        case 1:
            return std::make_unique<sorter::LimitOneSorter<Key, Value, Comparator>>(opts, comp);
        default:
            return std::make_unique<sorter::TopKSorter<Key, Value, Comparator>>(
                opts, comp, settings);
    }
}

template <typename Key, typename Value>
template <typename Comparator>
std::unique_ptr<Sorter<Key, Value>> Sorter<Key, Value>::makeFromExistingRanges(
    const std::string& fileName,
    const std::vector<SorterRange>& ranges,
    const SortOptions& opts,
    const Comparator& comp,
    const Settings& settings) {
    sorter::checkNoExternalSortOnMongos(opts);

    invariant(opts.limit == 0,
              str::stream() << "Creating a Sorter from existing ranges is only available with the "
                               "NoLimitSorter (limit 0), but got limit "
                            << opts.limit);

    return std::make_unique<sorter::NoLimitSorter<Key, Value, Comparator>>(
        fileName, ranges, opts, comp, settings);
}
}  // namespace mongo
#undef MONGO_LOGV2_DEFAULT_COMPONENT
