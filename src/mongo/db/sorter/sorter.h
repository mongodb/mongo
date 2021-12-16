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

#include "mongo/db/sorter/sorted_data_iterator.h"
#include "mongo/db/sorter/sorter_gen.h"

namespace mongo::sorter {
/**
 * The in-memory and external Sorter.
 *
 * The Sorter is templated on Key and Value types, each of which require the following public
 * members:
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
 * CompFn is a function that compares std::pair<Key, Value> and returns an int less than, equal to,
 * or greater than 0 depending on how the two pairs compare with the same semantics as memcmp.
 */
template <typename Key, typename Value>
class Sorter {
public:
    using Data = std::pair<Key, Value>;
    using Iterator = SortedDataIterator<Key, Value>;
    using CompFn = std::function<int(const Data&, const Data&)>;
    using Settings = std::pair<typename Key::SorterDeserializeSettings,
                               typename Value::SorterDeserializeSettings>;

    struct PersistedState {
        std::string fileName;
        std::vector<SorterRange> ranges;
    };

    explicit Sorter(const CompFn& comp) : _comp(comp) {}

    Sorter(const Sorter&) = delete;
    Sorter& operator=(const Sorter&) = delete;

    virtual ~Sorter() {}

    /**
     * Adds the key/value. The Sorter may make its own owned copy of the data.
     */
    virtual void add(const Key& key, const Value& value) = 0;

    /**
     * Adds the key/value which aleady own their underlying data. The Sorter may (or may not) take
     * advantage of this by not making a new copy of the data.
     */
    virtual void addOwned(Key&& key, Value&& value) {
        add(key, value);
    }

    /**
     * Returns an Iterator to iterate through the sorted data. The Sorter must outlive the returned
     * Iterator.
     *
     * If the Sorter does not spill to disk, the specified return policy determines whether the
     * Iterator returns data from the Sorter via move or via copy. Returning data via copy is useful
     * if you may later need the data to still be intact in order to be spilled.
     *
     * If the sorter does spill to disk, the specified return policy is ignored.
     */
    virtual std::unique_ptr<Iterator> done(
        typename Iterator::ReturnPolicy returnPolicy = Iterator::ReturnPolicy::kMove) = 0;

    virtual PersistedState persistDataForShutdown() {
        MONGO_UNREACHABLE;
    }

    virtual size_t numSpills() const {
        return 0;
    }

    size_t numSorted() const {
        return _numSorted;
    }

    uint64_t totalDataSizeSorted() const {
        return _totalDataSizeSorted;
    }

protected:
    const CompFn _comp;

    size_t _numSorted = 0;
    uint64_t _totalDataSizeSorted = 0;

    bool _done = false;
};
}  // namespace mongo::sorter
