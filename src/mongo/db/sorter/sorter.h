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
#include "mongo/util/assert_util.h"
#include "mongo/util/bufreader.h"
#include "mongo/util/modules.h"
#include "mongo/util/shared_buffer_fragment.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <queue>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <boost/filesystem/path.hpp>
#include <boost/optional.hpp>

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

namespace MONGO_MOD_PUB mongo {

/**
 * Runtime options that control the Sorter's behavior
 */
struct SortOptions {
    // The number of KV pairs to be returned. 0 indicates no limit.
    unsigned long long limit;

    // When in-memory memory usage exceeds this value, we try to spill to the underlying sorter
    // storage. This is approximate.
    size_t maxMemoryUsageBytes;
    static const size_t DefaultMaxMemoryUsageBytes = 64 * 1024 * 1024;

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

    SortOptions()
        : limit(0),
          maxMemoryUsageBytes(DefaultMaxMemoryUsageBytes),
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

namespace sorter {

/**
 * This is the sorted output iterator from the sorting framework.
 */
template <typename Key, typename Value>
class Iterator {
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

    // Returns the next key without advancing the iterator.
    virtual const Key& peek() = 0;

    virtual ~Iterator() = default;

    virtual SorterRange getRange() const = 0;

    /**
     * Returns true iff it is valid to call spill() method on this iterator.
     */
    virtual bool spillable() const = 0;

    /**
     * Spills not-yet-returned data to disk and returns a new iterator. Invalidates the current
     * iterator.
     */
    [[nodiscard]] virtual std::unique_ptr<sorter::Iterator<Key, Value>> spill(
        const SortOptions& opts, const typename Sorter<Key, Value>::Settings& settings) = 0;
};

template <typename Key, typename Value>
class IteratorBase : public Iterator<Key, Value> {
public:
    SorterRange getRange() const override {
        MONGO_UNREACHABLE_TASSERT(11617000);
    }

    bool spillable() const override {
        return false;
    }

    [[nodiscard]] std::unique_ptr<Iterator<Key, Value>> spill(
        const SortOptions& opts, const typename Sorter<Key, Value>::Settings& settings) override {
        MONGO_UNREACHABLE_TASSERT(9917200);
    }
};

template <typename Key, typename Value, typename Comparator>
class MergeIterator;

/**
 * Returns an iterator that merges the passed-in iterators.
 */
template <typename Key, typename Value, typename Comparator>
std::unique_ptr<MergeIterator<Key, Value, Comparator>> merge(
    std::span<std::shared_ptr<Iterator<Key, Value>>> iters,
    const SortOptions& opts,
    const Comparator& comp);

}  // namespace sorter

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
 * Appends a pre-sorted range of data to a given storage and hands back an Iterator over that
 * storage range.
 */
template <typename Key, typename Value>
class SortedStorageWriter {
    SortedStorageWriter(const SortedStorageWriter&) = delete;
    SortedStorageWriter& operator=(const SortedStorageWriter&) = delete;

public:
    typedef sorter::Iterator<Key, Value> Iterator;
    typedef std::pair<typename Key::SorterDeserializeSettings,
                      typename Value::SorterDeserializeSettings>
        Settings;

    explicit SortedStorageWriter(const SortOptions& opts,
                                 const Settings& settings,
                                 SorterChecksumCalculator checksumCalculator);
    virtual ~SortedStorageWriter() = default;

    virtual void addAlreadySorted(const Key&, const Value&) = 0;

    /**
     * If the storage has a buffer, flushes the buffer to the storage.
     * Closes the storage to which data was written to and closes it.
     *
     * No more data can be added via addAlreadySorted() after calling done().
     */
    virtual std::shared_ptr<Iterator> done() = 0;
    virtual std::unique_ptr<Iterator> doneUnique() = 0;

    /**
     * For sorter storages that use a buffer, the SortedStorageWriter organizes data into chunks,
     * with a chunk getting written to the output storage when it exceeds a maximum chunks size. A
     * SortedStorageWriter client can produce a short chunk by manually calling this function.
     *
     * If the sorter storage doesn't use a buffer, or no new data has been added since the last
     * chunk was written, this function is a no-op.
     */
    virtual void writeChunk() = 0;

protected:
    const Settings _settings;
    // Keeps track of the hash of all data objects spilled to disk. Passed to the corresponding
    // storage's iterator to ensure data has not been corrupted after reading from disk.
    SorterChecksumCalculator _checksumCalculator;

    SortOptions _opts;
};


namespace sorter {

/**
 * A pure virtual class where we provide the factory methods to create a writer or iterator for the
 * specific type of storage the sorter is using.
 */
template <typename Key, typename Value>
class Storage {
public:
    typedef std::pair<typename Key::SorterDeserializeSettings,
                      typename Value::SorterDeserializeSettings>
        Settings;

    virtual ~Storage() = default;

    virtual std::unique_ptr<SortedStorageWriter<Key, Value>> makeWriter(
        const SortOptions& opts, const Settings& settings) = 0;

    virtual size_t getIteratorSize() = 0;

    virtual size_t getBufferSize() = 0;
    /**
     * Reconstructs a sorter when resuming an index build, following persistFromShutdown.
     */
    virtual std::shared_ptr<Iterator<Key, Value>> getSortedIterator(const SorterRange& range,
                                                                    const Settings& settings) = 0;

    /**
     * Gets the storage identifier (e.g. file name, ident) to persist a Storage upon clean shutdown.
     */
    virtual std::string getStorageIdentifier() = 0;

    /**
     * Persists a Storage upon clean shutdown.
     */
    virtual void keep() = 0;

    /**
     * Gets the dbName. Relevant for encryption during index builds.
     */
    virtual boost::optional<DatabaseName> getDbName() = 0;

    /**
     * Gets the checksum version used for serialization/deserialization.
     */
    virtual SorterChecksumVersion getChecksumVersion() = 0;
};

template <typename Key, typename Value>
class StorageBase : public Storage<Key, Value> {
public:
    StorageBase(boost::optional<DatabaseName> dbName, SorterChecksumVersion checksumVersion)
        : _dbName(dbName), _checksumVersion(checksumVersion) {}

    boost::optional<DatabaseName> getDbName() override {
        return _dbName;
    };

    SorterChecksumVersion getChecksumVersion() override {
        return _checksumVersion;
    };

private:
    boost::optional<DatabaseName> _dbName;
    SorterChecksumVersion _checksumVersion;
};

}  // namespace sorter

/**
 * Data iterator over an Input stream used in the MergeIterator.
 */
template <typename Key, typename Value>
class MONGO_MOD_PRIVATE Stream {
public:
    typedef sorter::Iterator<Key, Value> Input;
    Stream(size_t sourceId, std::shared_ptr<Input> iter)
        : sourceId(sourceId), _current(iter->nextWithDeferredValue()), _rest(std::move(iter)) {}

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
    sorter::Iterator<Key, Value>& iterator() {
        return *_rest;
    }

    const size_t sourceId;

private:
    Key _current;
    std::shared_ptr<Input> _rest;
};

// 'Comparator' is a 3-way comparison, but std::priority_queue wants a '<' comparison.
// But also, std::priority_queue is a max-heap, and we want a min-heap.
// And also, 'Comparator' compares Keys, but std::priority_queue calls its comparator
// on whole elements.
//
// Used for the BoundedSorter and spillWithHeap in the sorter::Spiller class.
template <typename Key, typename Value, typename Comparator>
struct MONGO_MOD_PRIVATE Greater {

    // Prevent default construction.
    explicit Greater(Comparator const* compare) : compare(compare) {}

    bool operator()(const std::pair<Key, Value>& p1, const std::pair<Key, Value>& p2) const {
        return (*compare)(p1.first, p2.first) > 0;
    }
    Comparator const* compare;
};

/**
 * Validates that all ranges in a merge batch have non-decreasing offsets and form a contiguous
 * sequence.
 */
template <typename Key, typename Value>
void validateMergeSpillRanges(
    std::span<std::shared_ptr<sorter::Iterator<Key, Value>>> spillsToMerge) {
    invariant(!spillsToMerge.empty());
    int64_t expectedRangeStart = spillsToMerge.front()->getRange().getStart();
    for (const auto& it : spillsToMerge) {
        auto range = it->getRange();
        uassert(12017000,
                "Merge range end offset must be greater than or equal to start offset",
                range.getEnd() >= range.getStart());
        uassert(12017001,
                "Merge ranges in batch must be adjacent",
                range.getStart() == expectedRangeStart);
        expectedRangeStart = range.getEnd();
    }
}

namespace sorter {

/**
 * A class where we declare how to spill depending on the underlying storage the sorter is using.
 */
template <typename Key, typename Value, typename Comparator>
class Spiller {
public:
    typedef std::pair<typename Key::SorterDeserializeSettings,
                      typename Value::SorterDeserializeSettings>
        Settings;
    typedef std::pair<Key, Value> Data;

    virtual std::shared_ptr<Iterator<Key, Value>> spill(const SortOptions& opts,
                                                        const Settings& settings,
                                                        std::span<std::pair<Key, Value>> data) = 0;

    virtual std::unique_ptr<Iterator<Key, Value>> spillUnique(
        const SortOptions& opts,
        const Settings& settings,
        std::span<std::pair<Key, Value>> data) = 0;

    virtual std::shared_ptr<Iterator<Key, Value>> spillWithHeap(
        const SortOptions& opts,
        const Settings& settings,
        std::priority_queue<Data, std::vector<Data>, Greater<Key, Value, Comparator>>& heap) = 0;

    /**
     * Merge 'iters' in groups of at most 'maxSpillsPerMerge' until at most
     * 'numTargetedSpills' remain.
     *
     * 'iters' must be ordered by increasing range start offset and form one contiguous
     * range.
     */
    virtual void mergeSpills(const SortOptions& opts,
                             const Settings& settings,
                             SorterStats& stats,
                             std::vector<std::shared_ptr<Iterator<Key, Value>>>& iters,
                             Comparator comp,
                             std::size_t numTargetedSpills,
                             std::size_t maxSpillsPerMerge) = 0;

    virtual Storage<Key, Value>& getStorage() = 0;

    /**
     * Retrieves the directory where the storage is created for spilling data.
     */
    virtual boost::filesystem::path getSpillDir() = 0;

    virtual ~Spiller() = default;
};

template <typename Key, typename Value, typename Comparator>
class SpillerBase : public Spiller<Key, Value, Comparator> {
public:
    typedef std::pair<Key, Value> Data;
    using Settings = Spiller<Key, Value, Comparator>::Settings;

    SpillerBase(std::unique_ptr<Storage<Key, Value>> storage, int64_t minAvailableDiskBytesToSpill)
        : _storage(std::move(storage)),
          _minAvailableDiskBytesToSpill(minAvailableDiskBytesToSpill) {}

    std::shared_ptr<Iterator<Key, Value>> spill(const SortOptions& opts,
                                                const Settings& settings,
                                                std::span<std::pair<Key, Value>> data) override {
        return _spill(opts, settings, data)->done();
    }

    std::unique_ptr<Iterator<Key, Value>> spillUnique(
        const SortOptions& opts,
        const Settings& settings,
        std::span<std::pair<Key, Value>> data) override {
        return _spill(opts, settings, data)->doneUnique();
    }

    std::shared_ptr<Iterator<Key, Value>> spillWithHeap(
        const SortOptions& opts,
        const Settings& settings,
        std::priority_queue<Data, std::vector<Data>, Greater<Key, Value, Comparator>>& heap)
        override {
        std::unique_ptr<SortedStorageWriter<Key, Value>> writer =
            _storage->makeWriter(opts, settings);
        while (!heap.empty()) {
            writer->addAlreadySorted(heap.top().first, heap.top().second);
            heap.pop();
        }
        return writer->done();
    }

    Storage<Key, Value>& getStorage() override {
        return *_storage;
    }

protected:
    std::unique_ptr<Storage<Key, Value>> _storage;
    int64_t _minAvailableDiskBytesToSpill;

private:
    virtual std::unique_ptr<SortedStorageWriter<Key, Value>> _spill(
        const SortOptions& opts,
        const Settings& settings,
        std::span<std::pair<Key, Value>> data) = 0;
};

}  // namespace sorter

/**
 * Each instance of this class accepts (Key, Value) pairs and, depending on its SortOptions and the
 * configured Spiller/Storage, may keep them in memory or spill sorted ranges to an external
 * storage.
 *
 * Ownership and cleanup of any spill storage are handled by the underlying Storage implementation.
 * Callers that need spilled data to outlive the Sorter itself should use persistDataForShutdown()
 * and later reconstruct a Sorter from the returned PersistedState.
 */
template <typename Key, typename Value>
class Sorter : public SorterBase {
    Sorter(const Sorter&) = delete;
    Sorter& operator=(const Sorter&) = delete;

public:
    typedef std::pair<Key, Value> Data;
    typedef std::function<Value()> ValueProducer;
    typedef sorter::Iterator<Key, Value> Iterator;
    typedef std::pair<typename Key::SorterDeserializeSettings,
                      typename Value::SorterDeserializeSettings>
        Settings;

    struct PersistedState {
        std::string storageIdentifier;
        std::vector<SorterRange> ranges;
    };

    explicit Sorter(const SortOptions& opts);

    /**
     * ExtSort-only constructor. storageIdentifier is the file name or ident for a sorter::File or
     * container (respectively) in it's spill directory path.
     */
    Sorter(const SortOptions& opts, std::string storageIdentifier);

    template <typename Comparator>
    static std::unique_ptr<Sorter> make(
        const SortOptions& opts,
        const Comparator& comp,
        std::shared_ptr<sorter::Spiller<Key, Value, Comparator>> spiller,
        const Settings& settings);

    template <typename Comparator>
    static std::unique_ptr<Sorter> makeFromExistingRanges(
        std::string storageIdentifier,
        const std::vector<SorterRange>& ranges,
        const SortOptions& opts,
        const Comparator& comp,
        std::shared_ptr<sorter::Spiller<Key, Value, Comparator>> spiller,
        const Settings& settings);

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
     * Spills all of the sorted data to disk, preserves the temporary storage, and then returns
     * metadata which can be passed to makeFromExistingRanges() to use the spill storage later. May
     * be called before or after calling done().
     *
     * Only applicable to sorters with limit = 0.
     */
    virtual PersistedState persistDataForShutdown() {
        MONGO_UNREACHABLE;
    };

    SharedBufferFragmentBuilder& memPool() {
        invariant(_memPool);
        return _memPool.get();
    }

    virtual void spill() = 0;

protected:
    SortOptions _opts;

    std::vector<std::shared_ptr<Iterator>> _iters;  // Data that has already been spilled.

    boost::optional<SharedBufferFragmentBuilder> _memPool;
};


template <typename Key, typename Value>
class BoundedSorterInterface : public SorterBase {

public:
    explicit BoundedSorterInterface(const SortOptions& opts) : SorterBase(opts.sorterTracker) {}

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
    using Settings = sorter::Spiller<Key, Value, Comparator>::Settings;

    BoundedSorter(const SortOptions& opts,
                  Comparator comp,
                  BoundMaker makeBound,
                  std::shared_ptr<sorter::Spiller<Key, Value, Comparator>> spiller,
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

protected:
    std::shared_ptr<sorter::Spiller<Key, Value, Comparator>> _spiller;

private:
    using SpillIterator = sorter::Iterator<Key, Value>;

    void _spill(size_t maxMemoryUsageBytes);

    bool _checkInput;

    const SortOptions _opts;

    using Data = std::pair<Key, Value>;
    std::priority_queue<Data, std::vector<Data>, Greater<Key, Value, Comparator>> _heap;

    std::unique_ptr<SpillIterator> _spillIter;

    boost::optional<Key> _min;
    bool _done = false;
};
}  // namespace MONGO_MOD_PUB mongo
