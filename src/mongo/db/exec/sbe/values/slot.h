/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/values/value.h"

namespace mongo::sbe::value {
/**
 * Uniquely identifies a slot in an SBE plan. The slot ids are allocated as part of creating the
 * SBE plan, and remain constant during query runtime.
 *
 * Each slot id corresponds to a particular 'SlotAccessor' instance. The various slot accessor
 * implementations provide the mechanism for manipulating values held in slots.
 */
using SlotId = int64_t;

/**
 * Interface for accessing a value held inside a slot. SBE plans assign unique ids to each slot. The
 * id is needed to find the corresponding accessor during plan preparation, but need not be known by
 * the slot accessor object itself.
 */
class SlotAccessor {
public:
    virtual ~SlotAccessor() = default;

    /**
     * Returns a non-owning view of value currently stored in the slot. The returned value is valid
     * until the content of this slot changes (usually as a result of calling getNext()). If the
     * caller needs to hold onto the value longer then it must make a copy of the value.
     */
    virtual std::pair<TypeTags, Value> getViewOfValue() const = 0;

    /**
     * Sometimes it may be determined that a caller is the last one to access this slot. If that is
     * the case then the caller can use this optimized method to move out the value out of the slot
     * saving the extra copy operation. Not all slots own the values stored in them so they must
     * make a deep copy. The returned value is owned by the caller.
     */
    virtual std::pair<TypeTags, Value> copyOrMoveValue() = 0;
};

/**
 * Accessor for a slot which provides a view of a value that is owned elsewhere.
 */
class ViewOfValueAccessor final : public SlotAccessor {
public:
    /**
     * Returns non-owning view of the value.
     */
    std::pair<TypeTags, Value> getViewOfValue() const override {
        return {_tag, _val};
    }

    /**
     * Returns a copy of the value.
     */
    std::pair<TypeTags, Value> copyOrMoveValue() override {
        return copyValue(_tag, _val);
    }

    void reset() {
        reset(TypeTags::Nothing, 0);
    }

    void reset(TypeTags tag, Value val) {
        _tag = tag;
        _val = val;
    }

private:
    TypeTags _tag{TypeTags::Nothing};
    Value _val{0};
};

/**
 * Accessor for a slot which can own the value held by that slot.
 */
class OwnedValueAccessor final : public SlotAccessor {
public:
    OwnedValueAccessor() = default;

    OwnedValueAccessor(const OwnedValueAccessor& other) {
        if (other._owned) {
            auto [tag, val] = copyValue(other._tag, other._val);
            _tag = tag;
            _val = val;
            _owned = true;
        } else {
            _tag = other._tag;
            _val = other._val;
            _owned = false;
        }
    }

    OwnedValueAccessor(OwnedValueAccessor&& other) noexcept {
        _tag = other._tag;
        _val = other._val;
        _owned = other._owned;

        other._owned = false;
    }

    ~OwnedValueAccessor() {
        release();
    }

    // Copy and swap idiom for a single copy/move assignment operator.
    OwnedValueAccessor& operator=(OwnedValueAccessor other) noexcept {
        std::swap(_tag, other._tag);
        std::swap(_val, other._val);
        std::swap(_owned, other._owned);
        return *this;
    }

    /**
     * Returns a non-owning view of the value.
     */
    std::pair<TypeTags, Value> getViewOfValue() const override {
        return {_tag, _val};
    }

    /**
     * If a the value is owned by this slot, then the slot relinquishes ownership of the returned
     * value. Alternatively, if the value is unowned, then the caller receives a copy. Either way,
     * the caller owns the resulting value.
     */
    std::pair<TypeTags, Value> copyOrMoveValue() override {
        if (_owned) {
            _owned = false;
            return {_tag, _val};
        } else {
            return copyValue(_tag, _val);
        }
    }

    void reset() {
        reset(TypeTags::Nothing, 0);
    }

    void reset(TypeTags tag, Value val) {
        reset(true, tag, val);
    }

    void reset(bool owned, TypeTags tag, Value val) {
        release();

        _tag = tag;
        _val = val;
        _owned = owned;
    }

private:
    void release() {
        if (_owned) {
            releaseValue(_tag, _val);
            _owned = false;
        }
    }

    TypeTags _tag{TypeTags::Nothing};
    Value _val;
    bool _owned{false};
};

/**
 * An accessor for a slot which must hold an array-like type (e.g. 'TypeTags::Array' or
 * 'TypeTags::bsonArray'). The array is never owned by this slot. Provides an interface to iterate
 * over the values that constitute the array.
 *
 * It is illegal to fill out the slot with a type that is not array-like.
 */
class ArrayAccessor final : public SlotAccessor {
public:
    void reset(TypeTags tag, Value val) {
        _enumerator.reset(tag, val);
    }

    // Return non-owning view of the value.
    std::pair<TypeTags, Value> getViewOfValue() const override {
        return _enumerator.getViewOfValue();
    }
    std::pair<TypeTags, Value> copyOrMoveValue() override {
        // We can never move out values from array.
        auto [tag, val] = getViewOfValue();
        return copyValue(tag, val);
    }

    bool atEnd() const {
        return _enumerator.atEnd();
    }

    bool advance() {
        return _enumerator.advance();
    }

private:
    ArrayEnumerator _enumerator;
};

/**
 * Some SBE stages must materialize rows inside (key, value) data structures, e.g. for the sort or
 * hash aggregation operators. In such cases, both key and value are each materialized rows which
 * may consist of multiple 'sbe::Value' instances. This accessor provides a view of a particular
 * value inside a particular key for such a data structure.
 *
 * T is the type of an iterator pointing to the (key, value) pair of interest.
 */
template <typename T>
class MaterializedRowKeyAccessor final : public SlotAccessor {
public:
    MaterializedRowKeyAccessor(T& it, size_t slot) : _it(it), _slot(slot) {}

    std::pair<TypeTags, Value> getViewOfValue() const override {
        return _it->first._fields[_slot].getViewOfValue();
    }
    std::pair<TypeTags, Value> copyOrMoveValue() override {
        // We can never move out values from keys.
        auto [tag, val] = getViewOfValue();
        return copyValue(tag, val);
    }

private:
    T& _it;
    size_t _slot;
};

/**
 * Some SBE stages must materialize rows inside (key, value) data structures, e.g. for the sort or
 * hash aggregation operators. In such cases, both key and value are each materialized rows which
 * may consist of multiple 'sbe::Value' instances. This accessor provides a view of a particular
 * 'sbe::Value' inside the materialized row which serves as the "value" part of the (key, value)
 * pair.
 *
 * T is the type of an iterator pointing to the (key, value) pair of interest.
 */
template <typename T>
class MaterializedRowValueAccessor final : public SlotAccessor {
public:
    MaterializedRowValueAccessor(T& it, size_t slot) : _it(it), _slot(slot) {}

    std::pair<TypeTags, Value> getViewOfValue() const override {
        return _it->second._fields[_slot].getViewOfValue();
    }
    std::pair<TypeTags, Value> copyOrMoveValue() override {
        return _it->second._fields[_slot].copyOrMoveValue();
    }

    void reset(bool owned, TypeTags tag, Value val) {
        _it->second._fields[_slot].reset(owned, tag, val);
    }

private:
    T& _it;
    size_t _slot;
};

/**
 * Provides a view of  a particular slot inside a particular row of an abstract table-like container
 * of type T.
 */
template <typename T>
class MaterializedRowAccessor final : public SlotAccessor {
public:
    /**
     * Constructs an accessor for the row with index 'it' inside the given 'container'. Within that
     * row, the resulting accessor provides a vew of the value at the given 'slot'.
     */
    MaterializedRowAccessor(T& container, const size_t& it, size_t slot)
        : _container(container), _it(it), _slot(slot) {}

    std::pair<TypeTags, Value> getViewOfValue() const override {
        return _container[_it]._fields[_slot].getViewOfValue();
    }
    std::pair<TypeTags, Value> copyOrMoveValue() override {
        return _container[_it]._fields[_slot].copyOrMoveValue();
    }

    void reset(bool owned, TypeTags tag, Value val) {
        _container[_it]._fields[_slot].reset(owned, tag, val);
    }

private:
    T& _container;
    const size_t& _it;
    const size_t _slot;
};

struct MaterializedRow {
    void makeOwned() {
        for (auto& f : _fields) {
            auto [tag, val] = f.getViewOfValue();
            auto [copyTag, copyVal] = copyValue(tag, val);
            f.reset(copyTag, copyVal);
        }
    }

    bool operator==(const MaterializedRow& rhs) const {
        for (size_t idx = 0; idx < _fields.size(); ++idx) {
            auto [lhsTag, lhsVal] = _fields[idx].getViewOfValue();
            auto [rhsTag, rhsVal] = rhs._fields[idx].getViewOfValue();
            auto [tag, val] = compareValue(lhsTag, lhsVal, rhsTag, rhsVal);

            if (tag != TypeTags::NumberInt32 || val != 0) {
                return false;
            }
        }

        return true;
    }

    std::vector<OwnedValueAccessor> _fields;
};

struct MaterializedRowComparator {
    MaterializedRowComparator(const std::vector<value::SortDirection>& direction)
        : _direction(direction) {}

    bool operator()(const MaterializedRow& lhs, const MaterializedRow& rhs) const {
        for (size_t idx = 0; idx < lhs._fields.size(); ++idx) {
            auto [lhsTag, lhsVal] = lhs._fields[idx].getViewOfValue();
            auto [rhsTag, rhsVal] = rhs._fields[idx].getViewOfValue();
            auto [tag, val] = compareValue(lhsTag, lhsVal, rhsTag, rhsVal);
            if (tag != TypeTags::NumberInt32) {
                return false;
            }
            if (bitcastTo<int32_t>(val) < 0 && _direction[idx] == SortDirection::Ascending) {
                return true;
            }
            if (bitcastTo<int32_t>(val) > 0 && _direction[idx] == SortDirection::Descending) {
                return true;
            }
            if (bitcastTo<int32_t>(val) != 0) {
                return false;
            }
        }

        return false;
    }

    const std::vector<SortDirection>& _direction;
    // TODO - add collator and whatnot.
};

struct MaterializedRowHasher {
    std::size_t operator()(const MaterializedRow& k) const {
        size_t res = 17;
        for (auto& f : k._fields) {
            auto [tag, val] = f.getViewOfValue();
            res = res * 31 + hashValue(tag, val);
        }
        return res;
    }
};

/**
 * Commonly used containers.
 */
template <typename T>
using SlotMap = absl::flat_hash_map<SlotId, T>;
using SlotAccessorMap = SlotMap<SlotAccessor*>;
using FieldAccessorMap = absl::flat_hash_map<std::string, std::unique_ptr<ViewOfValueAccessor>>;
using SlotSet = absl::flat_hash_set<SlotId>;
using SlotVector = std::vector<SlotId>;
}  // namespace mongo::sbe::value
