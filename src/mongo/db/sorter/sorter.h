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

#include <deque>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "mongo/bson/util/builder.h"

/**
 * This is the public API for the Sorter (both in-memory and external)
 *
 * Many of the classes in this file are templated on Key and Value types which
 * require the following public members:
 *
 * // A type carrying extra information used by the deserializer. Contents are
 * // up to you, but  it should be cheap to copy. Use an empty struct if your
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
 * // For types with owned and unowned states, such as BSON, return an owned version.
 * // Return *this if your type doesn't have an unowned state
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

    // Directory into which we place a file when spilling to disk. Must be explicitly set if
    // extSortAllowed is true.
    std::string tempDir;

    SortOptions() : limit(0), maxMemoryUsageBytes(64 * 1024 * 1024), extSortAllowed(false) {}

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

    // Unowned objects are only valid until next call to any method

    virtual bool more() = 0;
    virtual std::pair<Key, Value> next() = 0;

    virtual ~SortIteratorInterface() {}

    // Returns an iterator that merges the passed in iterators
    template <typename Comparator>
    static SortIteratorInterface* merge(
        const std::vector<std::shared_ptr<SortIteratorInterface>>& iters,
        const std::string& fileName,
        const SortOptions& opts,
        const Comparator& comp);

    // Opens and closes the source of data over which this class iterates, if applicable.
    virtual void openSource() = 0;
    virtual void closeSource() = 0;

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

    template <typename Comparator>
    static Sorter* make(const SortOptions& opts,
                        const Comparator& comp,
                        const Settings& settings = Settings());

    virtual void add(const Key&, const Value&) = 0;

    /**
     * Cannot add more data after calling done().
     *
     * The returned Iterator must not outlive the Sorter instance, which manages file clean up.
     */
    virtual Iterator* done() = 0;

    virtual ~Sorter() {}

    bool usedDisk() {
        return _usedDisk;
    }

protected:
    bool _usedDisk{false};  // Keeps track of whether the sorter used disk or not
    Sorter() {}             // can only be constructed as a base
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
                              const std::string& fileName,
                              const std::streampos fileStartOffset,
                              const Settings& settings = Settings());

    void addAlreadySorted(const Key&, const Value&);

    /**
     * Spills any data remaining in the buffer to disk and then closes the file to which data was
     * written.
     *
     * No more data can be added via addAlreadySorted() after calling done().
     */
    Iterator* done();

    /**
     * Only call this after done() has been called to set the end offset.
     */
    std::streampos getFileEndOffset() {
        invariant(!_file.is_open());
        return _fileEndOffset;
    }

private:
    void spill();

    const Settings _settings;
    std::string _fileName;
    std::ofstream _file;
    BufBuilder _buffer;

    // Keeps track of the hash of all data objects spilled to disk. Passed to the FileIterator
    // to ensure data has not been corrupted after reading from disk.
    uint32_t _checksum = 0;

    // Tracks where in the file we started and finished writing the sorted data range so that the
    // information can be given to the Iterator in done(), and to the user via getFileEndOffset()
    // for the next SortedFileWriter instance using the same file.
    std::streampos _fileStartOffset;
    std::streampos _fileEndOffset;
};
}

/**
 * #include "mongo/db/sorter/sorter.cpp" and call this in a single translation
 * unit once for each unique set of template parameters.
 */
#define MONGO_CREATE_SORTER(Key, Value, Comparator)                                      \
    /* public classes */                                                                 \
    template class ::mongo::Sorter<Key, Value>;                                          \
    template class ::mongo::SortIteratorInterface<Key, Value>;                           \
    template class ::mongo::SortedFileWriter<Key, Value>;                                \
    /* internal classes */                                                               \
    template class ::mongo::sorter::NoLimitSorter<Key, Value, Comparator>;               \
    template class ::mongo::sorter::LimitOneSorter<Key, Value, Comparator>;              \
    template class ::mongo::sorter::TopKSorter<Key, Value, Comparator>;                  \
    template class ::mongo::sorter::MergeIterator<Key, Value, Comparator>;               \
    template class ::mongo::sorter::InMemIterator<Key, Value>;                           \
    template class ::mongo::sorter::FileIterator<Key, Value>;                            \
    /* factory functions */                                                              \
    template ::mongo::SortIteratorInterface<Key, Value>* ::mongo::                       \
        SortIteratorInterface<Key, Value>::merge<Comparator>(                            \
            const std::vector<std::shared_ptr<SortIteratorInterface>>& iters,            \
            const std::string& fileName,                                                 \
            const SortOptions& opts,                                                     \
            const Comparator& comp);                                                     \
    template ::mongo::Sorter<Key, Value>* ::mongo::Sorter<Key, Value>::make<Comparator>( \
        const SortOptions& opts, const Comparator& comp, const Settings& settings);
