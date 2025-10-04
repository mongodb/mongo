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

#include "mongo/bson/util/builder.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/sorter/sorter_checksum_calculator.h"
#include "mongo/db/sorter/sorter_gen.h"
#include "mongo/db/sorter/sorter_stats.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/bufreader.h"
#include "mongo/util/shared_buffer_fragment.h"

#include <cstddef>
#include <cstdint>
#include <fstream>  // IWYU pragma: keep
#include <functional>
#include <iterator>
#include <memory>
#include <queue>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <boost/filesystem/path.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

/**
 * This is the public API for the Sorter (both in-memory and external)
 *
 * Many of the classes in this file are templated on Key and Value types which
 * require the following public members:
 *
 * // A type carrying extra information used by the deserializer. Contents are
 * // up to you, but it should be cheap to copy. Use an empty struct if your
 * // deserializer doesn't need extra data.
 * struct SorterDeserializeSettings {};
 *
 * // Serialize this object to the BufBuilder
 * void serializeForSorter(BufBuilder& buf) const;
 *
 * // Deserialize and return an object from the BufReader
 * static Type deserializeForSorter(BufReader& buf, const Type::SorterDeserializeSettings&);
 *
 * // How much memory is used by your type? Include sizeof(*this) and any memory you reference.
 * int memUsageForSorter() const;
 *
 * // For types with owned and unowned states, such as BSON, return an owned version. The Sorter
 * // is responsible for converting any unowned data to an owned state if it needs to be buffered.
 * // Return *this if your type doesn't have an unowned state.
 * Type getOwned() const;
 *
 * Comparators are functors that that compare std::pair<Key, Value> and return an
 * int less than, equal to, or greater than 0 depending on how the two pairs
 * compare with the same semantics as memcmp.
 * Example for Key=BSONObj, Value=int:
 *
 * class MyComparator {
 * public:
 *     int operator()(const std::pair<BSONObj, int>& lhs,
 *                    const std::pair<BSONObj, int>& rhs) {
 *         int ret = lhs.first.woCompare(rhs.first, _ord);
 *         if (ret)
 *             return ret;
 *
 *        if (lhs.second >  rhs.second) return 1;
 *        if (lhs.second == rhs.second) return 0;
 *        return -1;
 *     }
 *     Ordering _ord;
 * };
 */

namespace mongo {

/**
 * Runtime options that control the Sorter's behavior
 */
struct SortOptions {
    // The number of KV pairs to be returned. 0 indicates no limit.
    unsigned long long limit;

    // When in-memory memory usage exceeds this value, we try to spill to disk. This is approximate.
    size_t maxMemoryUsageBytes;
    static const size_t DefaultMaxMemoryUsageBytes = 64 * 1024 * 1024;

    // In case the sorter spills encrypted data to disk that must be readable even after process
    // restarts, it must encrypt with a persistent key. This key is accessed using the database
    // name that the sorted collection lives in. If encryption is enabled and dbName is boost::none,
    // a temporary key is used.
    boost::optional<DatabaseName> dbName;

    // Directory into which we place a file when spilling to disk. boost::none means we aren't
    // allowing external sorting.
    boost::optional<boost::filesystem::path> tempDir;

    // If set, allows us to observe Sorter file handle usage.
    SorterFileStats* sorterFileStats;

    // If set, allows us to observe aggregate Sorter behaviors. The lifetime of this object must
    // exceed that of the Sorter instance; otherwise, it will lead to a user-after-free error.
    SorterTracker* sorterTracker;

    // When set, this sorter will own a memory pool that callers should used to allocate memory for
    // the keys we are sorting. If enabled, any values returned by memUsageForSorter() will be
    // ignored.
    bool useMemPool;

    // If set to true and sorted data fits into memory, sorted data will be moved into iterator
    // instead of copying.
    bool moveSortedDataIntoIterator;

    // Checksum version to use for spill files. Only applicable if tempDir != boost::none.
    SorterChecksumVersion checksumVersion = SorterChecksumVersion::v2;

    SortOptions()
        : limit(0),
          maxMemoryUsageBytes(DefaultMaxMemoryUsageBytes),
          tempDir(boost::none),
          sorterFileStats(nullptr),
          sorterTracker(nullptr),
          useMemPool(false),
          moveSortedDataIntoIterator(false) {}

    // Fluent API to support expressions like SortOptions().Limit(1000)

    SortOptions& Limit(unsigned long long newLimit) {
        limit = newLimit;
        return *this;
    }

    SortOptions& MaxMemoryUsageBytes(size_t newMaxMemoryUsageBytes) {
        maxMemoryUsageBytes = newMaxMemoryUsageBytes;
        return *this;
    }

    SortOptions& TempDir(boost::filesystem::path newTempDir) {
        tempDir = std::move(newTempDir);
        return *this;
    }

    SortOptions& DBName(DatabaseName newDbName) {
        dbName = std::move(newDbName);
        return *this;
    }

    SortOptions& FileStats(SorterFileStats* newSorterFileStats) {
        sorterFileStats = newSorterFileStats;
        return *this;
    }

    SortOptions& Tracker(SorterTracker* newSorterTracker) {
        sorterTracker = newSorterTracker;
        return *this;
    }

    SortOptions& MoveSortedDataIntoIterator(bool newMoveSortedDataIntoIterator = true) {
        moveSortedDataIntoIterator = newMoveSortedDataIntoIterator;
        return *this;
    }

    SortOptions& UseMemoryPool(bool usePool) {
        useMemPool = usePool;
        return *this;
    }

    SortOptions& ChecksumVersion(SorterChecksumVersion version) {
        checksumVersion = version;
        return *this;
    }
};

/**
 * This is a 0-sized dummy object that satisfies Sorter's Key/Value interface.
 */
class NullValue {
public:
    struct SorterDeserializeSettings {};  // unused
    void serializeForSorter(BufBuilder& buf) const {
        return;
    }
    static NullValue deserializeForSorter(BufReader& buf, const SorterDeserializeSettings&) {
        return {};
    }
    int memUsageForSorter() const {
        return 0;
    }
    NullValue getOwned() const {
        return {};
    }
    void makeOwned() {}
};

template <typename Key, typename Value>
class Sorter;

/**
 * This is the sorted output iterator from the sorting framework.
 */
template <typename Key, typename Value>
class SortIteratorInterface {
    SortIteratorInterface(const SortIteratorInterface&) = delete;
    SortIteratorInterface& operator=(const SortIteratorInterface&) = delete;

public:
    typedef std::pair<Key, Value> Data;
    typedef std::pair<typename Key::SorterDeserializeSettings,
                      typename Value::SorterDeserializeSettings>
        Settings;

    // Unowned objects are only valid until next call to any method

    virtual bool more() = 0;
    /**
     * Returns the new key-value pair.
     */
    virtual std::pair<Key, Value> next() = 0;

    /**
     * The following two methods are used together. nextWithDeferredValue() returns the next key. It
     * must be followed by a call to getDeferredValue(), to return the pending deferred value,
     * before calling next() or nextWithDeferredValue() again. This is intended specifically to
     * avoid allocating memory for the value if the caller eventually decides to abandon the
     * iterator and never consume any more values from it.
     */
    virtual Key nextWithDeferredValue() = 0;
    virtual Value getDeferredValue() = 0;

    virtual const Key& current() = 0;

    virtual ~SortIteratorInterface() {}

    // Returns an iterator that merges the passed in iterators
    template <typename Comparator>
    static std::unique_ptr<SortIteratorInterface> merge(
        std::span<std::shared_ptr<SortIteratorInterface>> iters,
        const SortOptions& opts,
        const Comparator& comp);

    virtual SorterRange getRange() const {
        invariant(false, "Only FileIterator has ranges");
        MONGO_UNREACHABLE;
    }

    /**
     * Returns true iff it is valid to call spill() method on this iterator.
     */
    virtual bool spillable() const {
        return false;
    }

    /**
     * Spills not-yet-returned data to disk and returns a new iterator. Invalidates the current
     * iterator.
     */
    [[nodiscard]] virtual std::unique_ptr<SortIteratorInterface<Key, Value>> spill(
        const SortOptions& opts, const typename Sorter<Key, Value>::Settings& settings) {
        MONGO_UNREACHABLE_TASSERT(9917200);
    }

protected:
    SortIteratorInterface() {}  // can only be constructed as a base
};

class SorterBase {
public:
    SorterBase(SorterTracker* sorterTracker = nullptr) : _stats(sorterTracker) {}
    ~SorterBase() {
        // After the Sorter is destroyed all memory it holds is gone so we are
        // setting the memory usage to zero to have it reflected on the sorterTracker
        // if it was provided.
        _stats.setMemUsage(0);
    }

    const SorterStats& stats() const {
        return _stats;
    }

protected:
    SorterStats _stats;
};


/**
 * Represents the file that a Sorter uses to spill to disk. Supports reading and writing
 * (append-only).
 */
class SorterFile {
public:
    SorterFile(boost::filesystem::path path, SorterFileStats* stats);
    ~SorterFile();

    const boost::filesystem::path& path() const {
        return _path;
    }

    /**
     * Signals that the on-disk storage should not be cleaned up.
     */
    void keep() {
        _keep = true;
    };

    /**
     * Reads the requested data from the storage. Cannot write more to the storage once this has
     * been called.
     */
    void read(std::streamoff offset, std::streamsize size, void* out);


    /**
     * Writes the given data to the end of the storage. Cannot be called after reading.
     */
    void write(const char* data, std::streamsize size);

    /**
     * Returns the current offset of the end of the storage. Cannot be called after reading.
     */
    std::streamoff currentOffset();

private:
    void _open();

    /**
     * Ensures that the file is open and that _offset is set to the end of the file.
     */
    void _ensureOpenForWriting();

    // The current offset of the end of the storage if there may be unflushed data, or -1 if the
    // file either has not yet been opened or has been flushed.
    std::streamoff _offset = -1;

    std::fstream _file;

    // Whether to keep the on-disk storage even after this in-memory object has been destructed.
    bool _keep = false;

    // If set, this points to an external metrics holder for tracking storage open/close
    // activity.
    SorterFileStats* _stats;

    boost::filesystem::path _path;
};

/**
 * This is the way to input data to the sorting framework.
 *
 * Each instance of this class will generate a file name and spill sorted data ranges to that file
 * if allowed in its given Settings. If the instance destructs before done() is called, it will
 * handle deleting the data file used for spills. Otherwise, if done() is called, responsibility for
 * file deletion moves to the returned Iterator object, which must then delete the file upon its own
 * destruction.
 */
template <typename Key, typename Value>
class Sorter : public SorterBase {
    Sorter(const Sorter&) = delete;
    Sorter& operator=(const Sorter&) = delete;

public:
    typedef std::pair<Key, Value> Data;
    typedef std::function<Value()> ValueProducer;
    typedef SortIteratorInterface<Key, Value> Iterator;
    typedef std::pair<typename Key::SorterDeserializeSettings,
                      typename Value::SorterDeserializeSettings>
        Settings;

    struct PersistedState {
        std::string fileName;
        std::vector<SorterRange> ranges;
    };

    explicit Sorter(const SortOptions& opts);

    /**
     * ExtSort-only constructor. fileName is the base name of a file in the temp directory.
     */
    Sorter(const SortOptions& opts, std::string fileName);

    template <typename Comparator>
    static std::unique_ptr<Sorter> make(const SortOptions& opts,
                                        const Comparator& comp,
                                        const Settings& settings = Settings());

    template <typename Comparator>
    static std::unique_ptr<Sorter> makeFromExistingRanges(std::string fileName,
                                                          const std::vector<SorterRange>& ranges,
                                                          const SortOptions& opts,
                                                          const Comparator& comp,
                                                          const Settings& settings = Settings());

    virtual void add(const Key&, const Value&) = 0;
    virtual void emplace(Key&&, ValueProducer) = 0;

    /**
     * Finishes inserting data and returns an iterator over the sorted results. Cannot add more data
     * after calling done().
     */
    virtual std::unique_ptr<Iterator> done() = 0;

    /**
     * Pauses loading and returns the iterator that can be used to get the current state. Clients of
     * this class can call this method to pause loading and get the current state available in
     * read-only mode for storing it to a persistent storage which is used by streaming query use
     * cases. New documents cannot be added until resume is called. The iterator returned is
     * reflecting current in memory state and is not guaranteed to be sorted.
     *
     * This cannot be called on sorters which have spilled state to disk.
     */
    virtual std::unique_ptr<Iterator> pause() = 0;

    /**
     * Resumes loading and cleans up internal state created during pause().
     */
    virtual void resume() = 0;

    virtual ~Sorter() {}

    /**
     * Spills all of the sorted data to disk, preserves the temporary file, and then returns
     * metadata which can be passed to makeFromExistingRanges() to use the spill file later. May be
     * called before or after calling done().
     *
     * Only applicable to sorters with limit = 0.
     */
    PersistedState persistDataForShutdown();

    SharedBufferFragmentBuilder& memPool() {
        invariant(_memPool);
        return _memPool.get();
    }

    virtual void spill() = 0;

protected:
    SortOptions _opts;

    std::shared_ptr<SorterFile> _file;

    std::vector<std::shared_ptr<Iterator>> _iters;  // Data that has already been spilled.

    boost::optional<SharedBufferFragmentBuilder> _memPool;
};


template <typename Key, typename Value>
class BoundedSorterInterface : public SorterBase {

public:
    BoundedSorterInterface(const SortOptions& opts) : SorterBase(opts.sorterTracker) {}

    virtual ~BoundedSorterInterface() {}

    // Feed one item of input to the sorter.
    // Together, add() and done() represent the input stream.
    virtual void add(Key key, Value value) = 0;

    // Indicate that no more input will arrive.
    // Together, add() and done() represent the input stream.
    virtual void done() = 0;

    // Prepare the sorter to receive a new stream of input.
    //
    // The new input stream is treated as unrelated to the old one: new elements are only compared
    // against each other, not against any elements of the old input stream.
    //
    // However, any SortOptions::limit applies to the entire sorter, not to each input stream
    // separately.
    virtual void restart() = 0;

    enum class State {
        // An output document is not available yet, but this may change as more input arrives.
        kWait,
        // An output document is available now: you may call next() once.
        kReady,
        // All output has been returned.
        kDone,
    };
    // Together, state() and next() represent the output stream.
    // See BoundedSorter::State for the meaning of each case.
    virtual State getState() const = 0;

    // Remove and return one item of output.
    // Only valid to call when getState() == kReady.
    // Together, state() and next() represent the output stream.
    virtual std::pair<Key, Value> next() = 0;

    // Serialize the bound for explain output
    virtual Document serializeBound(const SerializationOptions& opts) const = 0;

    virtual size_t limit() const = 0;

    // By default, uassert that the input meets our assumptions of being almost-sorted.
    // But if _checkInput is false, don't do that check.
    // The output will be in the wrong order but otherwise it should work.
    virtual bool checkInput() const = 0;

    virtual void forceSpill() = 0;

    // Update current bound without adding new item.
    virtual void setBound(Key key) = 0;
};

/**
 * Sorts data that is already "almost sorted", meaning we can put a bound on how out-of-order
 * any two input elements are. For example, maybe we are sorting by {time: 1} and we know that no
 * two documents are more than an hour out of order. This means as soon as we see {time: t}, we know
 * that any document earlier than {time: t - 1h} is safe to return.
 *
 * Note what's bounded is the difference in sort-key values, not the number of inversions.
 * This means we don't know how much space we'll need.
 *
 * This is not a subclass of Sorter because the interface is different: Sorter has a strict
 * separation between reading input and returning results, while BoundedSorter can alternate
 * between the two.
 *
 * Comparator does a 3-way comparison between two Keys: comp(x, y) < 0 iff x < y.
 *
 * BoundMaker takes a Key from the input, and computes a bound. The bound is a Key that is
 * less-or-equal to all future Keys that will be seen in the input.
 */
template <typename Key, typename Value, typename Comparator, typename BoundMaker>
class BoundedSorter final : public BoundedSorterInterface<Key, Value> {
public:
    // 'Comparator' is a 3-way comparison, but std::priority_queue wants a '<' comparison.
    // But also, std::priority_queue is a max-heap, and we want a min-heap.
    // And also, 'Comparator' compares Keys, but std::priority_queue calls its comparator
    // on whole elements.
    struct Greater {
        // Prevent default construction.
        explicit Greater(Comparator const* compare) : compare(compare) {}

        bool operator()(const std::pair<Key, Value>& p1, const std::pair<Key, Value>& p2) const {
            return (*compare)(p1.first, p2.first) > 0;
        }
        Comparator const* compare;
    };

    BoundedSorter(const SortOptions& opts,
                  Comparator comp,
                  BoundMaker makeBound,
                  bool checkInput = true);

    BoundedSorter(const BoundedSorter&) = delete;
    BoundedSorter(BoundedSorter&&) = delete;
    BoundedSorter& operator=(const BoundedSorter&) = delete;
    BoundedSorter& operator=(BoundedSorter&&) = delete;

    // Feed one item of input to the sorter.
    // Together, add() and done() represent the input stream.
    void add(Key key, Value value) override;

    // Indicate that no more input will arrive.
    // Together, add() and done() represent the input stream.
    void done() override {
        invariant(!_done);
        _done = true;
    }

    void restart() override;

    // Together, state() and next() represent the output stream.
    // See BoundedSorter::State for the meaning of each case.
    using State = typename BoundedSorterInterface<Key, Value>::State;
    State getState() const override;

    // Remove and return one item of output.
    // Only valid to call when getState() == kReady.
    // Together, state() and next() represent the output stream.
    std::pair<Key, Value> next() override;

    // Serialize the bound for explain output
    Document serializeBound(const SerializationOptions& opts) const override {
        return {makeBound.serialize(opts)};
    };

    size_t limit() const override {
        return _opts.limit;
    }

    bool checkInput() const override {
        return _checkInput;
    }

    void forceSpill() override {
        _spill(0 /*maxMemoryUsageBytes*/);
    }

    void setBound(Key key) override;

    const Comparator compare;
    const BoundMaker makeBound;

private:
    using SpillIterator = SortIteratorInterface<Key, Value>;

    void _spill(size_t maxMemoryUsageBytes);

    bool _checkInput;

    const SortOptions _opts;

    using KV = std::pair<Key, Value>;
    std::priority_queue<KV, std::vector<KV>, Greater> _heap;

    std::shared_ptr<SorterFile> _file;
    std::unique_ptr<SpillIterator> _spillIter;

    boost::optional<Key> _min;
    bool _done = false;
};

/**
 * Appends a pre-sorted range of data to a given storage and hands back an Iterator over that
 * storage range.
 */
template <typename Key, typename Value>
class SortedStorageWriter {
    SortedStorageWriter(const SortedStorageWriter&) = delete;
    SortedStorageWriter& operator=(const SortedStorageWriter&) = delete;

public:
    typedef SortIteratorInterface<Key, Value> Iterator;
    typedef std::pair<typename Key::SorterDeserializeSettings,
                      typename Value::SorterDeserializeSettings>
        Settings;

    explicit SortedStorageWriter(const SortOptions& opts, const Settings& settings);
    virtual ~SortedStorageWriter() = default;

    virtual void addAlreadySorted(const Key&, const Value&) = 0;

    /**
     * Writes any data remaining in the buffer to disk and then closes the storage to which data was
     * written.
     *
     * No more data can be added via addAlreadySorted() after calling done().
     */
    virtual std::shared_ptr<Iterator> done() = 0;
    virtual std::unique_ptr<Iterator> doneUnique() = 0;

    /**
     * The SortedStorageWriter organizes data into chunks, with a chunk getting written to the
     * output storage when it exceeds a maximum chunks size. A SortedStorageWriter client can
     * produce a short chunk by manually calling this function.
     *
     * If no new data has been added since the last chunk was written, this function is a no-op.
     */
    virtual void writeChunk() = 0;

protected:
    const Settings _settings;
    BufBuilder _buffer;

    // Keeps track of the hash of all data objects spilled to disk. Passed to the FileIterator
    // to ensure data has not been corrupted after reading from disk.
    SorterChecksumCalculator _checksumCalculator;

    SortOptions _opts;
};

template <typename Key, typename Value>
class SortedFileWriter : public SortedStorageWriter<Key, Value> {
public:
    typedef SortIteratorInterface<Key, Value> Iterator;
    typedef std::pair<typename Key::SorterDeserializeSettings,
                      typename Value::SorterDeserializeSettings>
        Settings;

    explicit SortedFileWriter(const SortOptions& opts,
                              std::shared_ptr<SorterFile> file,
                              const Settings& settings = Settings());

    ~SortedFileWriter() override = default;

    void addAlreadySorted(const Key&, const Value&) override;

    std::shared_ptr<Iterator> done() override;
    std::unique_ptr<Iterator> doneUnique() override;

    void writeChunk() override;

private:
    std::shared_ptr<SorterFile> _file;

    // Tracks where in the file we started writing the sorted data range so that the information can
    // be given to the Iterator in done().
    std::streamoff _fileStartOffset;
};
}  // namespace mongo
