/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/base/string_data_comparator.h"
#include "mongo/bson/util/builder.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/exec/sbe/values/slot_util.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/bufreader.h"

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
class BufReader;
class BufBuilder;
}  // namespace mongo

namespace mongo::sbe::value {

template <typename RowType>
class RowBase {
public:
    /**
     * Make deep copies of values stored in the buffer.
     */
    void makeOwned() {
        RowType& self = *static_cast<RowType*>(this);
        for (size_t idx = 0; idx < self.size(); ++idx) {
            makeOwned(idx);
        }
    }

    void makeOwned(size_t idx) {
        RowType& self = *static_cast<RowType*>(this);
        if (!self.owned()[idx]) {
            auto [tag, val] = value::copyValue(self.tags()[idx], self.values()[idx]);
            self.values()[idx] = val;
            self.tags()[idx] = tag;
            self.owned()[idx] = true;
        }
    }

    std::pair<value::TypeTags, value::Value> getViewOfValue(size_t idx) const {
        const RowType& self = *static_cast<const RowType*>(this);
        SlotAccessorHelper::dassertValidSlotValue(self.tags()[idx], self.values()[idx]);
        return {self.tags()[idx], self.values()[idx]};
    }

    std::pair<value::TypeTags, value::Value> copyOrMoveValue(size_t idx) {
        RowType& self = *static_cast<RowType*>(this);
        SlotAccessorHelper::dassertValidSlotValue(self.tags()[idx], self.values()[idx]);
        if (self.owned()[idx]) {
            self.owned()[idx] = false;
            return {self.tags()[idx], self.values()[idx]};
        } else {
            return value::copyValue(self.tags()[idx], self.values()[idx]);
        }
    }

    // 'idx' is the index of the column to reset.
    void reset(size_t idx, bool own, value::TypeTags tag, value::Value val) {
        RowType& self = *static_cast<RowType*>(this);
        if (self.owned()[idx]) {
            value::releaseValue(self.tags()[idx], self.values()[idx]);
            self.owned()[idx] = false;
        }
        self.values()[idx] = val;
        self.tags()[idx] = tag;
        self.owned()[idx] = own;
    }


    // The following methods are used by the sorter only.
    struct SorterDeserializeSettings {
        const CollatorInterface* collator{nullptr};
    };
    static void deserializeForSorterIntoRow(BufReader&, const SorterDeserializeSettings&, RowType&);
    static RowType deserializeForSorter(BufReader& buf, const SorterDeserializeSettings&);
    void serializeForSorter(BufBuilder& buf) const;
    int memUsageForSorter() const;
    auto getOwned() const {
        auto result = *static_cast<const RowType*>(this);
        result.makeOwned();
        return result;
    }

    /**
     * With these functions, an SBE value can be saved as a KeyString. This functionality is
     * intended for spilling key values used in the HashAgg stage. The format is not guaranteed to
     * be stable between versions, so it should not be used for long-term storage or communication
     * between instances.
     *
     * If 'numPrefixValsToRead' is provided, then only the given number of values from 'keyString'
     * are decoded into the resulting 'MaterializedRow'. The remaining suffix values in the
     * 'keyString' are ignored.
     *
     * If non-null 'collator' is provided during serialization, then any strings in the row are
     * encoded as ICU collation keys prior to being KeyString-encoded.
     */
    static RowType deserializeFromKeyString(
        const key_string::Value& keyString,
        BufBuilder* valueBufferBuilder,
        boost::optional<size_t> numPrefixValsToRead = boost::none);
    void serializeIntoKeyString(key_string::Builder& builder,
                                const CollatorInterface* collator = nullptr) const;

protected:
    void release() noexcept {
        RowType& self = *static_cast<RowType*>(this);
        for (size_t idx = 0; idx < self.size(); ++idx) {
            if (self.owned()[idx]) {
                value::releaseValue(self.tags()[idx], self.values()[idx]);
                self.owned()[idx] = false;
            }
        }
    }

    /**
     * Makes a deep copy on the incoming row.
     */
    void copy(const RowType& other) {
        RowType& self = *static_cast<RowType*>(this);
        invariant(self.size() == other.size());

        for (size_t idx = 0; idx < self.size(); ++idx) {
            if (other.owned()[idx]) {
                auto [tag, val] = value::copyValue(other.tags()[idx], other.values()[idx]);
                self.values()[idx] = val;
                self.tags()[idx] = tag;
                self.owned()[idx] = true;
            } else {
                self.values()[idx] = other.values()[idx];
                self.tags()[idx] = other.tags()[idx];
                self.owned()[idx] = false;
            }
        }
    }
};


/**
 * This class holds values in a buffer. The most common usage is a sort and hash agg plan stages. A
 * materialized row must only be used to store owned (deep) value copies.
 */
class MaterializedRow : public RowBase<MaterializedRow> {
    friend class RowBase<MaterializedRow>;

public:
    MaterializedRow(size_t count = 0) {
        resize(count);
    }

    MaterializedRow(const MaterializedRow& other) {
        resize(other.size());
        copy(other);
    }

    MaterializedRow(MaterializedRow&& other) noexcept {
        swap(*this, other);
    }

    ~MaterializedRow() {
        if (_state.data) {
            release();
            delete[] _state.data;
        }
    }

    MaterializedRow& operator=(MaterializedRow&& other) noexcept {
        swap(*this, other);
        return *this;
    }

    MaterializedRow& operator=(const MaterializedRow& other) {
        if (this == &other)
            return *this;

        MaterializedRow temp(other);
        swap(*this, temp);
        return *this;
    }

    MONGO_COMPILER_ALWAYS_INLINE size_t size() const {
        return _state.count;
    }

    MONGO_COMPILER_ALWAYS_INLINE bool isEmpty() {
        return _state.count == 0 ? true : false;
    }

    void resize(size_t count) {
        if (_state.data) {
            release();
            delete[] _state.data;
            _state.data = nullptr;
            _state.count = 0;
        }
        if (count) {
            _state.data = new char[sizeInBytes(count)];
            _state.count = count;
            auto valuePtr = values();
            auto tagPtr = tags();
            auto ownedPtr = owned();
            while (count--) {
                *valuePtr++ = 0;
                *tagPtr++ = TypeTags::Nothing;
                *ownedPtr++ = false;
            }
        }
    }

private:
    static size_t sizeInBytes(size_t count) {
        return count * (sizeof(value::Value) + sizeof(value::TypeTags) + sizeof(bool));
    }

    MONGO_COMPILER_ALWAYS_INLINE value::Value* values() const {
        return reinterpret_cast<value::Value*>(_state.data);
    }

    MONGO_COMPILER_ALWAYS_INLINE value::TypeTags* tags() const {
        return reinterpret_cast<value::TypeTags*>(_state.data +
                                                  _state.count * sizeof(value::Value));
    }

    MONGO_COMPILER_ALWAYS_INLINE bool* owned() const {
        return reinterpret_cast<bool*>(
            _state.data + _state.count * (sizeof(value::Value) + sizeof(value::TypeTags)));
    }

    MONGO_COMPILER_ALWAYS_INLINE friend void swap(MaterializedRow& lhs,
                                                  MaterializedRow& rhs) noexcept {
        std::swap(lhs._state, rhs._state);
    }

    // Tie members into a State struct to optimize swap.
    struct State {
        char* data;
        size_t count;
    };

    State _state{nullptr, 0};
};

template <size_t N>
class FixedSizeRow : public RowBase<FixedSizeRow<N>> {
    friend class RowBase<FixedSizeRow<N>>;

public:
    FixedSizeRow(size_t size = N) {
        invariant(size == N);
    }

    FixedSizeRow(const FixedSizeRow<N>& other) {
        RowBase<FixedSizeRow<N>>::copy(other);
    }

    FixedSizeRow(FixedSizeRow<N>&& other) noexcept {
        swap(*this, other);
    }

    FixedSizeRow<N>& operator=(const FixedSizeRow<N>& other) {
        RowBase<FixedSizeRow<N>>::release();
        RowBase<FixedSizeRow<N>>::copy(other);
        return *this;
    }

    FixedSizeRow<N>& operator=(FixedSizeRow<N>&& other) noexcept {
        swap(*this, other);
        return *this;
    }

    ~FixedSizeRow() {
        RowBase<FixedSizeRow<N>>::release();
    }

    constexpr size_t size() const {
        return N;
    }

    constexpr bool isEmpty() {
        return N == 0 ? true : false;
    }

    void resize(size_t count) {
        invariant(count == N);
    }

private:
    MONGO_COMPILER_ALWAYS_INLINE value::Value* values() {
        return _state.values;
    }

    MONGO_COMPILER_ALWAYS_INLINE const value::Value* values() const {
        return _state.values;
    }

    MONGO_COMPILER_ALWAYS_INLINE value::TypeTags* tags() {
        return _state.tags;
    }

    MONGO_COMPILER_ALWAYS_INLINE const value::TypeTags* tags() const {
        return _state.tags;
    }

    MONGO_COMPILER_ALWAYS_INLINE bool* owned() {
        return _state.owned;
    }

    MONGO_COMPILER_ALWAYS_INLINE const bool* owned() const {
        return _state.owned;
    }

    MONGO_COMPILER_ALWAYS_INLINE friend void swap(FixedSizeRow<N>& lhs,
                                                  FixedSizeRow<N>& rhs) noexcept {
        if constexpr (N > 0) {
            std::swap(lhs._state, rhs._state);
        }
    }

    // Tie members into a State struct to optimize swap.
    struct State {
        bool owned[N];
        value::TypeTags tags[N];
        value::Value values[N];
    };

    State _state{{false}, {value::TypeTags::Nothing}, {0}};
};


template <typename RowType>
struct RowEq {
    using ComparatorType = StringDataComparator;

    explicit RowEq(const ComparatorType* comparator = nullptr) : _comparator(comparator) {}

    bool operator()(const RowType& lhs, const RowType& rhs) const {
        for (size_t idx = 0; idx < lhs.size(); ++idx) {
            auto [lhsTag, lhsVal] = lhs.getViewOfValue(idx);
            auto [rhsTag, rhsVal] = rhs.getViewOfValue(idx);
            auto [tag, val] = compareValue(lhsTag, lhsVal, rhsTag, rhsVal, _comparator);

            if (tag != value::TypeTags::NumberInt32 || value::bitcastTo<int32_t>(val) != 0) {
                return false;
            }
        }

        return true;
    }

private:
    const ComparatorType* _comparator = nullptr;
};
typedef RowEq<MaterializedRow> MaterializedRowEq;

template <typename RowType>
struct RowLess {
public:
    RowLess(const std::vector<value::SortDirection>& sortDirs) {
        _sortDirs.reserve(sortDirs.size());
        for (auto&& dir : sortDirs) {
            // Store directions 'Ascending' as -1 and 'Descending' as 1 so that we can compare the
            // result of 'compareValue()' on the two pairs of tags & vals directly to the sort
            // direction.
            _sortDirs.push_back(dir == value::SortDirection::Ascending ? -1 : 1);
        }
    }

    bool operator()(const RowType& lhs, const RowType& rhs) const {
        for (size_t idx = 0; idx < lhs.size(); ++idx) {
            auto [lhsTag, lhsVal] = lhs.getViewOfValue(idx);
            auto [rhsTag, rhsVal] = rhs.getViewOfValue(idx);
            auto [tag, val] = compareValue(lhsTag, lhsVal, rhsTag, rhsVal);

            if (tag != value::TypeTags::NumberInt32 ||
                value::bitcastTo<int32_t>(val) != _sortDirs[idx]) {
                return false;
            }
        }

        return true;
    }

private:
    std::vector<int8_t> _sortDirs;
};
typedef RowLess<MaterializedRow> MaterializedRowLess;


template <typename RowType>
struct RowHasher {
    using CollatorType = CollatorInterface*;

    explicit RowHasher(const CollatorType collator = nullptr) : _collator(collator) {}

    std::size_t operator()(const RowType& k) const {
        size_t res = hashInit();
        for (size_t idx = 0; idx < k.size(); ++idx) {
            auto [tag, val] = k.getViewOfValue(idx);
            res = hashCombine(res, hashValue(tag, val, _collator));
        }
        return res;
    }

private:
    const CollatorType _collator = nullptr;
};

typedef RowHasher<MaterializedRow> MaterializedRowHasher;

int getApproximateSize(TypeTags tag, Value val);

typedef std::conditional<true, int, int> myint;

}  // namespace mongo::sbe::value
