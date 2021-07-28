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

#include <third_party/murmurhash3/MurmurHash3.h>

#include <boost/filesystem/path.hpp>
#include <deque>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "mongo/bson/util/builder.h"
#include "mongo/db/sorter/sorter_gen.h"
#include "mongo/util/bufreader.h"

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

    // Whether we are allowed to spill to disk. If this is false and in-memory exceeds
    // maxMemoryUsageBytes, we will uassert.
    bool extSortAllowed;

    // In case the sorter spills encrypted data to disk that must be readable even after process
    // restarts, it must encrypt with a persistent key. This key is accessed using the database
    // name that the sorted collection lives in. If encryption is enabled and dbName is boost::none,
    // a temporary key is used.
    boost::optional<std::string> dbName;

    // Directory into which we place a file when spilling to disk. Must be explicitly set if
    // extSortAllowed is true.
    std::string tempDir;

    // If set to true and sorted data fits into memory, sorted data will be moved into iterator
    // instead of copying.
    bool moveSortedDataIntoIterator;

    SortOptions()
        : limit(0),
          maxMemoryUsageBytes(64 * 1024 * 1024),
          extSortAllowed(false),
          moveSortedDataIntoIterator(false) {}

    // Fluent API to support expressions like SortOptions().Limit(1000).ExtSortAllowed(true)

    SortOptions& Limit(unsigned long long newLimit) {
        limit = newLimit;
        return *this;
    }

    SortOptions& MaxMemoryUsageBytes(size_t newMaxMemoryUsageBytes) {
        maxMemoryUsageBytes = newMaxMemoryUsageBytes;
        return *this;
    }

    SortOptions& ExtSortAllowed(bool newExtSortAllowed = true) {
        extSortAllowed = newExtSortAllowed;
        return *this;
    }

    SortOptions& TempDir(const std::string& newTempDir) {
        tempDir = newTempDir;
        return *this;
    }

    SortOptions& DBName(std::string newDbName) {
        dbName = std::move(newDbName);
        return *this;
    }

    SortOptions& MoveSortedDataIntoIterator(bool newMoveSortedDataIntoIterator = true) {
        moveSortedDataIntoIterator = newMoveSortedDataIntoIterator;
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
};

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
    virtual std::pair<Key, Value> next() = 0;

    virtual ~SortIteratorInterface() {}

    // Returns an iterator that merges the passed in iterators
    template <typename Comparator>
    static SortIteratorInterface* merge(
        const std::vector<std::shared_ptr<SortIteratorInterface>>& iters,
        const SortOptions& opts,
        const Comparator& comp);

    // Opens and closes the source of data over which this class iterates, if applicable.
    virtual void openSource() = 0;
    virtual void closeSource() = 0;

    virtual SorterRange getRange() const {
        invariant(false, "Only FileIterator has ranges");
        MONGO_UNREACHABLE;
    }

protected:
    SortIteratorInterface() {}  // can only be constructed as a base
};

/**
 * This is the way to input data to the sorting framework.
 *
 * Each instance of this class will generate a file name and spill sorted data ranges to that file
 * if allowed in its given Settings. If the instance destructs before done() is called, it will
 * handle deleting the data file used for spills. Otherwise, if done() is called, responsibility for
 * file deletion moves to the returned Iterator object, which must then delete the file upon its own
 * destruction.
 *
 * All users of Sorter implementations must define their own nextFileName() function to generate
 * unique file names for spills to disk. This is necessary because the sorter.cpp file is separately
 * directly included in multiple places, rather than compiled in one place and linked, and so cannot
 * itself provide a globally unique ID for file names. See existing function implementations of
 * nextFileName() for example.
 */
template <typename Key, typename Value>
class Sorter {
    Sorter(const Sorter&) = delete;
    Sorter& operator=(const Sorter&) = delete;

public:
    typedef std::pair<Key, Value> Data;
    typedef SortIteratorInterface<Key, Value> Iterator;
    typedef std::pair<typename Key::SorterDeserializeSettings,
                      typename Value::SorterDeserializeSettings>
        Settings;

    struct PersistedState {
        std::string fileName;
        std::vector<SorterRange> ranges;
    };

    /**
     * Represents the file that a Sorter uses to spill to disk. Supports reading after writing (or
     * reading without any writing), but does not support writing after any reading has been done.
     */
    class File {
    public:
        File(std::string path) : _path(std::move(path)) {
            invariant(!_path.empty());
        }

        ~File();

        const boost::filesystem::path& path() const {
            return _path;
        }

        /**
         * Signals that the on-disk file should not be cleaned up.
         */
        void keep() {
            _keep = true;
        };

        /**
         * Reads the requested data from the file. Cannot write more to the file once this has been
         * called.
         */
        void read(std::streamoff offset, std::streamsize size, void* out);

        /**
         * Writes the given data to the end of the file. Cannot be called after reading.
         */
        void write(const char* data, std::streamsize size);

        /**
         * Returns the current offset of the end of the file. Cannot be called after reading.
         */
        std::streamoff currentOffset();

    private:
        void _open();

        void _ensureOpenForWriting();

        boost::filesystem::path _path;
        std::fstream _file;

        // The current offset of the end of the file, or -1 if the file either has not yet been
        // opened or is already being read.
        std::streamoff _offset = -1;

        // Whether to keep the on-disk file even after this in-memory object has been destructed.
        bool _keep = false;
    };

    explicit Sorter(const SortOptions& opts);

    /**
     * ExtSort-only constructor. fileName is the base name of a file in the temp directory.
     */
    Sorter(const SortOptions& opts, const std::string& fileName);

    template <typename Comparator>
    static Sorter* make(const SortOptions& opts,
                        const Comparator& comp,
                        const Settings& settings = Settings());

    template <typename Comparator>
    static Sorter* makeFromExistingRanges(const std::string& fileName,
                                          const std::vector<SorterRange>& ranges,
                                          const SortOptions& opts,
                                          const Comparator& comp,
                                          const Settings& settings = Settings());

    virtual void add(const Key&, const Value&) = 0;
    virtual void emplace(Key&& k, Value&& v) {
        add(k, v);
    }
    /**
     * Cannot add more data after calling done().
     */
    virtual Iterator* done() = 0;

    virtual ~Sorter() {}

    size_t numSpills() const {
        return _numSpills;
    }

    size_t numSorted() const {
        return _numSorted;
    }

    uint64_t totalDataSizeSorted() const {
        return _totalDataSizeSorted;
    }

    PersistedState persistDataForShutdown();

protected:
    Sorter() {}  // can only be constructed as a base

    virtual void spill() = 0;

    size_t _numSpills = 0;  // Keeps track of the number of times data was spilled to disk.
    size_t _numSorted = 0;  // Keeps track of the number of keys sorted.
    uint64_t _totalDataSizeSorted = 0;  // Keeps track of the total size of data sorted.

    SortOptions _opts;

    std::shared_ptr<File> _file;

    std::vector<std::shared_ptr<Iterator>> _iters;  // Data that has already been spilled.
};

/**
 * Appends a pre-sorted range of data to a given file and hands back an Iterator over that file
 * range.
 */
template <typename Key, typename Value>
class SortedFileWriter {
    SortedFileWriter(const SortedFileWriter&) = delete;
    SortedFileWriter& operator=(const SortedFileWriter&) = delete;

public:
    typedef SortIteratorInterface<Key, Value> Iterator;
    typedef std::pair<typename Key::SorterDeserializeSettings,
                      typename Value::SorterDeserializeSettings>
        Settings;

    explicit SortedFileWriter(const SortOptions& opts,
                              std::shared_ptr<typename Sorter<Key, Value>::File> file,
                              const Settings& settings = Settings());

    void addAlreadySorted(const Key&, const Value&);

    /**
     * Spills any data remaining in the buffer to disk and then closes the file to which data was
     * written.
     *
     * No more data can be added via addAlreadySorted() after calling done().
     */
    Iterator* done();

private:
    void spill();

    const Settings _settings;
    std::shared_ptr<typename Sorter<Key, Value>::File> _file;
    BufBuilder _buffer;

    // Keeps track of the hash of all data objects spilled to disk. Passed to the FileIterator
    // to ensure data has not been corrupted after reading from disk.
    uint32_t _checksum = 0;

    // Tracks where in the file we started writing the sorted data range so that the information can
    // be given to the Iterator in done().
    std::streamoff _fileStartOffset;

    boost::optional<std::string> _dbName;
};
}  // namespace mongo

/**
 * #include "mongo/db/sorter/sorter.cpp" and call this in a single translation
 * unit once for each unique set of template parameters.
 */
#define MONGO_CREATE_SORTER(Key, Value, Comparator)                                            \
    /* public classes */                                                                       \
    template class ::mongo::Sorter<Key, Value>;                                                \
    template class ::mongo::SortIteratorInterface<Key, Value>;                                 \
    template class ::mongo::SortedFileWriter<Key, Value>;                                      \
    /* internal classes */                                                                     \
    template class ::mongo::sorter::NoLimitSorter<Key, Value, Comparator>;                     \
    template class ::mongo::sorter::LimitOneSorter<Key, Value, Comparator>;                    \
    template class ::mongo::sorter::TopKSorter<Key, Value, Comparator>;                        \
    template class ::mongo::sorter::MergeIterator<Key, Value, Comparator>;                     \
    template class ::mongo::sorter::InMemIterator<Key, Value>;                                 \
    template class ::mongo::sorter::FileIterator<Key, Value>;                                  \
    /* factory functions */                                                                    \
    template ::mongo::SortIteratorInterface<Key, Value>* ::mongo::                             \
        SortIteratorInterface<Key, Value>::merge<Comparator>(                                  \
            const std::vector<std::shared_ptr<SortIteratorInterface>>& iters,                  \
            const SortOptions& opts,                                                           \
            const Comparator& comp);                                                           \
    template ::mongo::Sorter<Key, Value>* ::mongo::Sorter<Key, Value>::make<Comparator>(       \
        const SortOptions& opts, const Comparator& comp, const Settings& settings);            \
    template ::mongo::Sorter<Key, Value>* ::mongo::Sorter<Key, Value>::makeFromExistingRanges< \
        Comparator>(const std::string& fileName,                                               \
                    const std::vector<SorterRange>& ranges,                                    \
                    const SortOptions& opts,                                                   \
                    const Comparator& comp,                                                    \
                    const Settings& settings);
