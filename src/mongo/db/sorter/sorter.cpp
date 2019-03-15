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

/**
 * This is the implementation for the Sorter.
 *
 * It is intended to be included in other cpp files like this:
 *
 * #include <normal/include/files.h>
 *
 * #include "mongo/db/sorter/sorter.h"
 *
 * namespace mongo {
 *     // Your code
 * }
 *
 * #include "mongo/db/sorter/sorter.cpp"
 * MONGO_CREATE_SORTER(MyKeyType, MyValueType, MyComparatorType);
 *
 * Do this once for each unique set of parameters to MONGO_CREATE_SORTER.
 */

#include "mongo/db/sorter/sorter.h"

#include <boost/filesystem/operations.hpp>
#include <snappy.h>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/config.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/encryption_hooks.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/overflow_arithmetic.h"
#include "mongo/s/is_mongos.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/bufreader.h"
#include "mongo/util/destructor_guard.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/unowned_ptr.h"

namespace mongo {

namespace sorter {

using std::shared_ptr;
using namespace mongoutils;

// We need to use the "real" errno everywhere, not GetLastError() on Windows
inline std::string myErrnoWithDescription() {
    int errnoCopy = errno;
    StringBuilder sb;
    sb << "errno:" << errnoCopy << ' ' << strerror(errnoCopy);
    return sb.str();
}

template <typename Data, typename Comparator>
void dassertCompIsSane(const Comparator& comp, const Data& lhs, const Data& rhs) {
#if defined(MONGO_CONFIG_DEBUG_BUILD) && !defined(_MSC_VER)
    // MSVC++ already does similar verification in debug mode in addition to using
    // algorithms that do more comparisons. Doing our own verification in addition makes
    // debug builds considerably slower without any additional safety.

    // test reversed comparisons
    const int regular = comp(lhs, rhs);
    if (regular == 0) {
        invariant(comp(rhs, lhs) == 0);
    } else if (regular < 0) {
        invariant(comp(rhs, lhs) > 0);
    } else {
        invariant(comp(rhs, lhs) < 0);
    }

    // test reflexivity
    invariant(comp(lhs, lhs) == 0);
    invariant(comp(rhs, rhs) == 0);
#endif
}

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

    void openSource() {}
    void closeSource() {}

    bool more() {
        return !_data.empty();
    }
    Data next() {
        Data out = _data.front();
        _data.pop_front();
        return out;
    }

private:
    std::deque<Data> _data;
};

/**
 * Returns results from a sorted range within a file. Each instance is given a file name and start
 * and end offsets.
 *
 * This class is NOT responsible for file clean up / deletion. There are openSource() and
 * closeSource() functions to ensure the FileIterator is not holding the file open when the file is
 * deleted. Since it is one among many FileIterators, it cannot close a file that may still be in
 * use elsewhere.
 */
template <typename Key, typename Value>
class FileIterator : public SortIteratorInterface<Key, Value> {
public:
    typedef std::pair<typename Key::SorterDeserializeSettings,
                      typename Value::SorterDeserializeSettings>
        Settings;
    typedef std::pair<Key, Value> Data;

    FileIterator(const std::string& fileName,
                 std::streampos fileStartOffset,
                 std::streampos fileEndOffset,
                 const Settings& settings)
        : _settings(settings),
          _done(false),
          _fileName(fileName),
          _fileStartOffset(fileStartOffset),
          _fileEndOffset(fileEndOffset) {
        uassert(16815,
                str::stream() << "unexpected empty file: " << _fileName,
                boost::filesystem::file_size(_fileName) != 0);
    }

    void openSource() {
        _file.open(_fileName.c_str(), std::ios::in | std::ios::binary);
        uassert(16814,
                str::stream() << "error opening file \"" << _fileName << "\": "
                              << myErrnoWithDescription(),
                _file.good());
        _file.seekg(_fileStartOffset);
        uassert(50979,
                str::stream() << "error seeking starting offset of '" << _fileStartOffset
                              << "' in file \""
                              << _fileName
                              << "\": "
                              << myErrnoWithDescription(),
                _file.good());
    }

    void closeSource() {
        _file.close();
        uassert(50969,
                str::stream() << "error closing file \"" << _fileName << "\": "
                              << myErrnoWithDescription(),
                !_file.fail());
    }

    bool more() {
        if (!_done)
            fillBufferIfNeeded();  // may change _done
        return !_done;
    }

    Data next() {
        verify(!_done);
        fillBufferIfNeeded();

        // Note: calling read() on the _bufferReader buffer in the deserialize function advances the
        // buffer. Since Key comes before Value in the _bufferReader, and C++ makes no function
        // parameter evaluation order guarantees, we cannot deserialize Key and Value straight into
        // the Data constructor
        auto first = Key::deserializeForSorter(*_bufferReader, _settings.first);
        auto second = Value::deserializeForSorter(*_bufferReader, _settings.second);
        return Data(std::move(first), std::move(second));
    }

private:
    /**
     * Attempts to refill the _bufferReader if it is empty. Expects _done to be false.
     */
    void fillBufferIfNeeded() {
        verify(!_done);

        if (!_bufferReader || _bufferReader->atEof())
            fillBufferFromDisk();
    }

    /**
     * Tries to read from disk and places any results in _bufferReader. If there is no more data to
     * read, then _done is set to true and the function returns immediately.
     */
    void fillBufferFromDisk() {
        int32_t rawSize;
        read(&rawSize, sizeof(rawSize));
        if (_done)
            return;

        // negative size means compressed
        const bool compressed = rawSize < 0;
        int32_t blockSize = std::abs(rawSize);

        _buffer.reset(new char[blockSize]);
        read(_buffer.get(), blockSize);
        uassert(16816, "file too short?", !_done);

        auto encryptionHooks = EncryptionHooks::get(getGlobalServiceContext());
        if (encryptionHooks->enabled()) {
            std::unique_ptr<char[]> out(new char[blockSize]);
            size_t outLen;
            Status status =
                encryptionHooks->unprotectTmpData(reinterpret_cast<uint8_t*>(_buffer.get()),
                                                  blockSize,
                                                  reinterpret_cast<uint8_t*>(out.get()),
                                                  blockSize,
                                                  &outLen);
            uassert(28841,
                    str::stream() << "Failed to unprotect data: " << status.toString(),
                    status.isOK());
            blockSize = outLen;
            _buffer.swap(out);
        }

        if (!compressed) {
            _bufferReader.reset(new BufReader(_buffer.get(), blockSize));
            return;
        }

        dassert(snappy::IsValidCompressedBuffer(_buffer.get(), blockSize));

        size_t uncompressedSize;
        uassert(17061,
                "couldn't get uncompressed length",
                snappy::GetUncompressedLength(_buffer.get(), blockSize, &uncompressedSize));

        std::unique_ptr<char[]> decompressionBuffer(new char[uncompressedSize]);
        uassert(17062,
                "decompression failed",
                snappy::RawUncompress(_buffer.get(), blockSize, decompressionBuffer.get()));

        // hold on to decompressed data and throw out compressed data at block exit
        _buffer.swap(decompressionBuffer);
        _bufferReader.reset(new BufReader(_buffer.get(), uncompressedSize));
    }

    /**
     * Attempts to read data from disk. Sets _done to true when file offset reaches _fileEndOffset.
     *
     * Masserts on any file errors
     */
    void read(void* out, size_t size) {
        invariant(_file.is_open());

        const std::streampos offset = _file.tellg();
        uassert(51049,
                str::stream() << "error reading file \"" << _fileName << "\": "
                              << myErrnoWithDescription(),
                offset >= 0);

        if (offset >= _fileEndOffset) {
            invariant(offset == _fileEndOffset);
            _done = true;
            return;
        }

        _file.read(reinterpret_cast<char*>(out), size);
        uassert(16817,
                str::stream() << "error reading file \"" << _fileName << "\": "
                              << myErrnoWithDescription(),
                _file.good());
        verify(_file.gcount() == static_cast<std::streamsize>(size));
    }

    const Settings _settings;
    bool _done;
    std::unique_ptr<char[]> _buffer;
    std::unique_ptr<BufReader> _bufferReader;
    std::string _fileName;            // File containing the sorted data range.
    std::streampos _fileStartOffset;  // File offset at which the sorted data range starts.
    std::streampos _fileEndOffset;    // File offset at which the sorted data range ends.
    std::ifstream _file;
};

/**
 * Merge-sorts results from 0 or more FileIterators, all of which should be iterating over sorted
 * ranges within the same file. This class is given the data source file name upon construction and
 * is responsible for deleting the data source file upon destruction.
 */
template <typename Key, typename Value, typename Comparator>
class MergeIterator : public SortIteratorInterface<Key, Value> {
public:
    typedef SortIteratorInterface<Key, Value> Input;
    typedef std::pair<Key, Value> Data;

    MergeIterator(const std::vector<std::shared_ptr<Input>>& iters,
                  const std::string& itersSourceFileName,
                  const SortOptions& opts,
                  const Comparator& comp)
        : _opts(opts),
          _remaining(opts.limit ? opts.limit : std::numeric_limits<unsigned long long>::max()),
          _first(true),
          _greater(comp),
          _itersSourceFileName(itersSourceFileName) {
        for (size_t i = 0; i < iters.size(); i++) {
            iters[i]->openSource();
            if (iters[i]->more()) {
                _heap.push_back(std::make_shared<Stream>(i, iters[i]->next(), iters[i]));
            } else {
                iters[i]->closeSource();
            }
        }

        if (_heap.empty()) {
            _remaining = 0;
            return;
        }

        std::make_heap(_heap.begin(), _heap.end(), _greater);
        std::pop_heap(_heap.begin(), _heap.end(), _greater);
        _current = _heap.back();
        _heap.pop_back();
    }

    ~MergeIterator() {
        // Clear the remaining Stream objects first, to close the file handles before deleting the
        // file. Some systems will error closing the file if any file handles are still open.
        _current.reset();
        _heap.clear();
        DESTRUCTOR_GUARD(boost::filesystem::remove(_itersSourceFileName));
    }

    void openSource() {}
    void closeSource() {}

    bool more() {
        if (_remaining > 0 && (_first || !_heap.empty() || _current->more()))
            return true;

        _remaining = 0;
        return false;
    }

    Data next() {
        verify(_remaining);

        _remaining--;

        if (_first) {
            _first = false;
            return _current->current();
        }

        if (!_current->advance()) {
            verify(!_heap.empty());
            std::pop_heap(_heap.begin(), _heap.end(), _greater);
            _current = _heap.back();
            _heap.pop_back();
        } else if (!_heap.empty() && _greater(_current, _heap.front())) {
            std::pop_heap(_heap.begin(), _heap.end(), _greater);
            std::swap(_current, _heap.back());
            std::push_heap(_heap.begin(), _heap.end(), _greater);
        }

        return _current->current();
    }


private:
    /**
     * Data iterator over an Input stream.
     *
     * This class is responsible for closing the Input source upon destruction, unfortunately,
     * because that is the path of least resistence to a design change requiring MergeIterator to
     * handle eventual deletion of said Input source.
     */
    class Stream {
    public:
        Stream(size_t fileNum, const Data& first, std::shared_ptr<Input> rest)
            : fileNum(fileNum), _current(first), _rest(rest) {}

        ~Stream() {
            _rest->closeSource();
        }

        const Data& current() const {
            return _current;
        }
        bool more() {
            return _rest->more();
        }
        bool advance() {
            if (!_rest->more())
                return false;

            _current = _rest->next();
            return true;
        }

        const size_t fileNum;

    private:
        Data _current;
        std::shared_ptr<Input> _rest;
    };

    class STLComparator {  // uses greater rather than less-than to maintain a MinHeap
    public:
        explicit STLComparator(const Comparator& comp) : _comp(comp) {}
        bool operator()(unowned_ptr<const Stream> lhs, unowned_ptr<const Stream> rhs) const {
            // first compare data
            dassertCompIsSane(_comp, lhs->current(), rhs->current());
            int ret = _comp(lhs->current(), rhs->current());
            if (ret)
                return ret > 0;

            // then compare fileNums to ensure stability
            return lhs->fileNum > rhs->fileNum;
        }

    private:
        const Comparator _comp;
    };

    SortOptions _opts;
    unsigned long long _remaining;
    bool _first;
    std::shared_ptr<Stream> _current;
    std::vector<std::shared_ptr<Stream>> _heap;  // MinHeap
    STLComparator _greater;                      // named so calls make sense
    std::string _itersSourceFileName;
};

template <typename Key, typename Value, typename Comparator>
class NoLimitSorter : public Sorter<Key, Value> {
public:
    typedef std::pair<Key, Value> Data;
    typedef SortIteratorInterface<Key, Value> Iterator;
    typedef std::pair<typename Key::SorterDeserializeSettings,
                      typename Value::SorterDeserializeSettings>
        Settings;

    NoLimitSorter(const SortOptions& opts,
                  const Comparator& comp,
                  const Settings& settings = Settings())
        : _comp(comp), _settings(settings), _opts(opts), _memUsed(0) {
        verify(_opts.limit == 0);
        if (_opts.extSortAllowed) {
            _fileName = _opts.tempDir + "/" + nextFileName();
        }
    }

    ~NoLimitSorter() {
        if (!_done) {
            // If done() was never called to return a MergeIterator, then this Sorter still owns
            // file deletion.
            DESTRUCTOR_GUARD(boost::filesystem::remove(_fileName));
        }
    }

    void add(const Key& key, const Value& val) {
        invariant(!_done);

        _data.push_back(std::make_pair(key, val));

        _memUsed += key.memUsageForSorter();
        _memUsed += val.memUsageForSorter();

        if (_memUsed > _opts.maxMemoryUsageBytes)
            spill();
    }

    Iterator* done() {
        invariant(!_done);

        if (_iters.empty()) {
            sort();
            return new InMemIterator<Key, Value>(_data);
        }

        spill();
        Iterator* mergeIt = Iterator::merge(_iters, _fileName, _opts, _comp);
        _done = true;
        return mergeIt;
    }

private:
    class STLComparator {
    public:
        explicit STLComparator(const Comparator& comp) : _comp(comp) {}
        bool operator()(const Data& lhs, const Data& rhs) const {
            dassertCompIsSane(_comp, lhs, rhs);
            return _comp(lhs, rhs) < 0;
        }

    private:
        const Comparator& _comp;
    };

    void sort() {
        STLComparator less(_comp);
        std::stable_sort(_data.begin(), _data.end(), less);

        // Does 2x more compares than stable_sort
        // TODO test on windows
        // std::sort(_data.begin(), _data.end(), comp);
    }

    void spill() {
        invariant(!_done);

        this->_usedDisk = true;
        if (_data.empty())
            return;

        if (!_opts.extSortAllowed) {
            // XXX This error message is only correct for aggregation, but it is also the
            // only way this code could be hit at the moment. If the Sorter is used
            // elsewhere where extSortAllowed could possibly be false, this message will
            // need to be revisited.
            uasserted(16819,
                      str::stream()
                          << "Sort exceeded memory limit of "
                          << _opts.maxMemoryUsageBytes
                          << " bytes, but did not opt in to external sorting. Aborting operation."
                          << " Pass allowDiskUse:true to opt in.");
        }

        sort();

        SortedFileWriter<Key, Value> writer(
            _opts, _fileName, _nextSortedFileWriterOffset, _settings);
        for (; !_data.empty(); _data.pop_front()) {
            writer.addAlreadySorted(_data.front().first, _data.front().second);
        }
        Iterator* iteratorPtr = writer.done();
        _nextSortedFileWriterOffset = writer.getFileEndOffset();

        _iters.push_back(std::shared_ptr<Iterator>(iteratorPtr));

        _memUsed = 0;
    }

    const Comparator _comp;
    const Settings _settings;
    SortOptions _opts;
    std::string _fileName;
    std::streampos _nextSortedFileWriterOffset = 0;
    bool _done = false;
    size_t _memUsed;
    std::deque<Data> _data;                         // the "current" data
    std::vector<std::shared_ptr<Iterator>> _iters;  // data that has already been spilled
};

template <typename Key, typename Value, typename Comparator>
class LimitOneSorter : public Sorter<Key, Value> {
    // Since this class is only used for limit==1, it omits all logic to
    // spill to disk and only tracks memory usage if explicitly requested.
public:
    typedef std::pair<Key, Value> Data;
    typedef SortIteratorInterface<Key, Value> Iterator;

    LimitOneSorter(const SortOptions& opts, const Comparator& comp)
        : _comp(comp), _haveData(false) {
        verify(opts.limit == 1);
    }

    void add(const Key& key, const Value& val) {
        Data contender(key, val);

        if (_haveData) {
            dassertCompIsSane(_comp, _best, contender);
            if (_comp(_best, contender) <= 0)
                return;  // not good enough
        } else {
            _haveData = true;
        }

        _best = contender;
    }

    Iterator* done() {
        if (_haveData) {
            return new InMemIterator<Key, Value>(_best);
        } else {
            return new InMemIterator<Key, Value>();
        }
    }

private:
    const Comparator _comp;
    Data _best;
    bool _haveData;  // false at start, set to true on first call to add()
};

template <typename Key, typename Value, typename Comparator>
class TopKSorter : public Sorter<Key, Value> {
public:
    typedef std::pair<Key, Value> Data;
    typedef SortIteratorInterface<Key, Value> Iterator;
    typedef std::pair<typename Key::SorterDeserializeSettings,
                      typename Value::SorterDeserializeSettings>
        Settings;

    TopKSorter(const SortOptions& opts,
               const Comparator& comp,
               const Settings& settings = Settings())
        : _comp(comp),
          _settings(settings),
          _opts(opts),
          _memUsed(0),
          _haveCutoff(false),
          _worstCount(0),
          _medianCount(0) {
        // This also *works* with limit==1 but LimitOneSorter should be used instead
        verify(_opts.limit > 1);

        if (_opts.extSortAllowed) {
            _fileName = _opts.tempDir + "/" + nextFileName();
        }

        // Preallocate a fixed sized vector of the required size if we don't expect it to have a
        // major impact on our memory budget. This is the common case with small limits.
        if (opts.limit <
            std::min((opts.maxMemoryUsageBytes / 10) / sizeof(typename decltype(_data)::value_type),
                     _data.max_size())) {
            _data.reserve(opts.limit);
        }
    }

    ~TopKSorter() {
        if (!_done) {
            // If done() was never called to return a MergeIterator, then this Sorter still owns
            // file deletion.
            DESTRUCTOR_GUARD(boost::filesystem::remove(_fileName));
        }
    }

    void add(const Key& key, const Value& val) {
        invariant(!_done);

        STLComparator less(_comp);
        Data contender(key, val);

        if (_data.size() < _opts.limit) {
            if (_haveCutoff && !less(contender, _cutoff))
                return;

            _data.push_back(contender);

            _memUsed += key.memUsageForSorter();
            _memUsed += val.memUsageForSorter();

            if (_data.size() == _opts.limit)
                std::make_heap(_data.begin(), _data.end(), less);

            if (_memUsed > _opts.maxMemoryUsageBytes)
                spill();

            return;
        }

        verify(_data.size() == _opts.limit);

        if (!less(contender, _data.front()))
            return;  // not good enough

        // Remove the old worst pair and insert the contender, adjusting _memUsed

        _memUsed += key.memUsageForSorter();
        _memUsed += val.memUsageForSorter();

        _memUsed -= _data.front().first.memUsageForSorter();
        _memUsed -= _data.front().second.memUsageForSorter();

        std::pop_heap(_data.begin(), _data.end(), less);
        _data.back() = contender;
        std::push_heap(_data.begin(), _data.end(), less);

        if (_memUsed > _opts.maxMemoryUsageBytes)
            spill();
    }

    Iterator* done() {
        if (_iters.empty()) {
            sort();
            return new InMemIterator<Key, Value>(_data);
        }

        spill();
        Iterator* iterator = Iterator::merge(_iters, _fileName, _opts, _comp);
        _done = true;
        return iterator;
    }

private:
    class STLComparator {
    public:
        explicit STLComparator(const Comparator& comp) : _comp(comp) {}
        bool operator()(const Data& lhs, const Data& rhs) const {
            dassertCompIsSane(_comp, lhs, rhs);
            return _comp(lhs, rhs) < 0;
        }

    private:
        const Comparator& _comp;
    };

    void sort() {
        STLComparator less(_comp);

        if (_data.size() == _opts.limit) {
            std::sort_heap(_data.begin(), _data.end(), less);
        } else {
            std::stable_sort(_data.begin(), _data.end(), less);
        }
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

        STLComparator less(_comp);  // less is "better" for TopK.

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
        if (_worstCount >= _opts.limit) {
            if (!_haveCutoff || less(_worstSeen, _cutoff)) {
                _cutoff = _worstSeen;
                _haveCutoff = true;
            }
            _worstCount = 0;
        }
        if (_medianCount >= _opts.limit) {
            if (!_haveCutoff || less(_lastMedian, _cutoff)) {
                _cutoff = _lastMedian;
                _haveCutoff = true;
            }
            _medianCount = 0;
        }
    }

    void spill() {
        invariant(!_done);

        this->_usedDisk = true;
        if (_data.empty())
            return;

        if (!_opts.extSortAllowed) {
            // XXX This error message is only correct for aggregation, but it is also the
            // only way this code could be hit at the moment. If the Sorter is used
            // elsewhere where extSortAllowed could possibly be false, this message will
            // need to be revisited.
            uasserted(16820,
                      str::stream()
                          << "Sort exceeded memory limit of "
                          << _opts.maxMemoryUsageBytes
                          << " bytes, but did not opt in to external sorting. Aborting operation."
                          << " Pass allowDiskUse:true to opt in.");
        }

        // We should check readOnly before getting here.
        invariant(!storageGlobalParams.readOnly);

        sort();
        updateCutoff();

        SortedFileWriter<Key, Value> writer(
            _opts, _fileName, _nextSortedFileWriterOffset, _settings);
        for (size_t i = 0; i < _data.size(); i++) {
            writer.addAlreadySorted(_data[i].first, _data[i].second);
        }

        // clear _data and release backing array's memory
        std::vector<Data>().swap(_data);

        Iterator* iteratorPtr = writer.done();
        _nextSortedFileWriterOffset = writer.getFileEndOffset();
        _iters.push_back(std::shared_ptr<Iterator>(iteratorPtr));

        _memUsed = 0;
    }

    const Comparator _comp;
    const Settings _settings;
    SortOptions _opts;
    std::string _fileName;
    std::streampos _nextSortedFileWriterOffset = 0;
    bool _done = false;
    size_t _memUsed;
    std::vector<Data> _data;  // the "current" data. Organized as max-heap if size == limit.
    std::vector<std::shared_ptr<Iterator>> _iters;  // data that has already been spilled

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
// SortedFileWriter
//

template <typename Key, typename Value>
SortedFileWriter<Key, Value>::SortedFileWriter(const SortOptions& opts,
                                               const std::string& fileName,
                                               const std::streampos fileStartOffset,
                                               const Settings& settings)
    : _settings(settings) {
    namespace str = mongoutils::str;

    // This should be checked by consumers, but if we get here don't allow writes.
    uassert(
        16946, "Attempting to use external sort from mongos. This is not allowed.", !isMongos());

    uassert(17148,
            "Attempting to use external sort without setting SortOptions::tempDir",
            !opts.tempDir.empty());

    boost::filesystem::create_directories(opts.tempDir);

    _fileName = fileName;

    // We open the provided file in append mode so that SortedFileWriter instances can share the
    // same file, used serially. We want to share files in order to stay below system open file
    // limits.
    _file.open(_fileName.c_str(), std::ios::binary | std::ios::app | std::ios::out);
    uassert(16818,
            str::stream() << "error opening file \"" << _fileName << "\": "
                          << sorter::myErrnoWithDescription(),
            _file.good());
    // The file descriptor is positioned at the end of a file when opened in append mode, but
    // _file.tellp() is not initialized on all systems to reflect this. Therefore, we must also pass
    // in the expected offset to this constructor.
    _fileStartOffset = fileStartOffset;

    // throw on failure
    _file.exceptions(std::ios::failbit | std::ios::badbit | std::ios::eofbit);
}

template <typename Key, typename Value>
void SortedFileWriter<Key, Value>::addAlreadySorted(const Key& key, const Value& val) {
    key.serializeForSorter(_buffer);
    val.serializeForSorter(_buffer);

    if (_buffer.len() > 64 * 1024)
        spill();
}

template <typename Key, typename Value>
void SortedFileWriter<Key, Value>::spill() {
    namespace str = mongoutils::str;

    int32_t size = _buffer.len();
    char* outBuffer = _buffer.buf();

    if (size == 0)
        return;

    std::string compressed;
    snappy::Compress(outBuffer, size, &compressed);
    verify(compressed.size() <= size_t(std::numeric_limits<int32_t>::max()));

    const bool shouldCompress = compressed.size() < size_t(_buffer.len() / 10 * 9);
    if (shouldCompress) {
        size = compressed.size();
        outBuffer = const_cast<char*>(compressed.data());
    }

    std::unique_ptr<char[]> out;
    auto encryptionHooks = EncryptionHooks::get(getGlobalServiceContext());
    if (encryptionHooks->enabled()) {
        size_t protectedSizeMax = size + encryptionHooks->additionalBytesForProtectedBuffer();
        out.reset(new char[protectedSizeMax]);
        size_t resultLen;
        Status status = encryptionHooks->protectTmpData(reinterpret_cast<const uint8_t*>(outBuffer),
                                                        size,
                                                        reinterpret_cast<uint8_t*>(out.get()),
                                                        protectedSizeMax,
                                                        &resultLen);
        uassert(28842,
                str::stream() << "Failed to compress data: " << status.toString(),
                status.isOK());
        outBuffer = out.get();
        size = resultLen;
    }

    // negative size means compressed
    size = shouldCompress ? -size : size;
    try {
        _file.write(reinterpret_cast<const char*>(&size), sizeof(size));
        _file.write(outBuffer, std::abs(size));
    } catch (const std::exception&) {
        msgasserted(16821,
                    str::stream() << "error writing to file \"" << _fileName << "\": "
                                  << sorter::myErrnoWithDescription());
    }

    _buffer.reset();
}

template <typename Key, typename Value>
SortIteratorInterface<Key, Value>* SortedFileWriter<Key, Value>::done() {
    spill();
    std::streampos currentFileOffset = _file.tellp();
    uassert(50980,
            str::stream() << "error fetching current file descriptor offset in file \"" << _fileName
                          << "\": "
                          << sorter::myErrnoWithDescription(),
            currentFileOffset >= 0);

    // In case nothing was written to disk, use _fileStartOffset because tellp() may not be
    // initialized on all systems upon opening the file.
    _fileEndOffset = currentFileOffset < _fileStartOffset ? _fileStartOffset : currentFileOffset;
    _file.close();

    return new sorter::FileIterator<Key, Value>(
        _fileName, _fileStartOffset, _fileEndOffset, _settings);
}

//
// Factory Functions
//

template <typename Key, typename Value>
template <typename Comparator>
SortIteratorInterface<Key, Value>* SortIteratorInterface<Key, Value>::merge(
    const std::vector<std::shared_ptr<SortIteratorInterface>>& iters,
    const std::string& fileName,
    const SortOptions& opts,
    const Comparator& comp) {
    return new sorter::MergeIterator<Key, Value, Comparator>(iters, fileName, opts, comp);
}

template <typename Key, typename Value>
template <typename Comparator>
Sorter<Key, Value>* Sorter<Key, Value>::make(const SortOptions& opts,
                                             const Comparator& comp,
                                             const Settings& settings) {
    // This should be checked by consumers, but if it isn't try to fail early.
    uassert(16947,
            "Attempting to use external sort from mongos. This is not allowed.",
            !(isMongos() && opts.extSortAllowed));

    uassert(17149,
            "Attempting to use external sort without setting SortOptions::tempDir",
            !(opts.extSortAllowed && opts.tempDir.empty()));
    switch (opts.limit) {
        case 0:
            return new sorter::NoLimitSorter<Key, Value, Comparator>(opts, comp, settings);
        case 1:
            return new sorter::LimitOneSorter<Key, Value, Comparator>(opts, comp);
        default:
            return new sorter::TopKSorter<Key, Value, Comparator>(opts, comp, settings);
    }
}
}
