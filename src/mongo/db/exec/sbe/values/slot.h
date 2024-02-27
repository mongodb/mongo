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

#include <type_traits>

#include "mongo/bson/bsonobj.h"
#include "mongo/config.h"
#include "mongo/db/exec/sbe/values/row.h"
#include "mongo/db/exec/sbe/values/slot_util.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/util/id_generator.h"

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
     * Returns an owned copy of the value currently stored in the slot.
     */
    inline std::pair<TypeTags, Value> getCopyOfValue() const {
        auto [tag, val] = getViewOfValue();
        return sbe::value::copyValue(tag, val);
    }

    /**
     * Sometimes it may be determined that a caller is the last one to access this slot. If that is
     * the case then the caller can use this optimized method to move out the value out of the slot
     * saving the extra copy operation. Not all slots own the values stored in them so they must
     * make a deep copy. The returned value is owned by the caller.
     */
    virtual std::pair<TypeTags, Value> copyOrMoveValue() = 0;

    template <typename T>
    bool is() const {
        return dynamic_cast<const T*>(this) != nullptr;
    }

    template <typename T>
    T* as() {
        return dynamic_cast<T*>(this);
    }

    template <typename T>
    const T* as() const {
        return dynamic_cast<const T*>(this);
    }
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
        SlotAccessorHelper::dassertValidSlotValue(_tag, _val);
        return {_tag, _val};
    }

    /**
     * If a the value is owned by this slot, then the slot relinquishes ownership of the returned
     * value. Alternatively, if the value is unowned, then the caller receives a copy. Either way,
     * the caller owns the resulting value.
     */
    std::pair<TypeTags, Value> copyOrMoveValue() override {
        SlotAccessorHelper::dassertValidSlotValue(_tag, _val);
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

    void makeOwned() {
        if (_owned) {
            return;
        }

        std::tie(_tag, _val) = copyValue(_tag, _val);
        _owned = true;
    }

private:
    void release() {
        if (_owned) {
            releaseValue(_tag, _val);
            _owned = false;
        }
    }

    bool _owned{false};
    TypeTags _tag{TypeTags::Nothing};
    Value _val{0};
};  // class OwnedValueAccessor

/**
 * An accessor for a slot which must hold an array-like type (e.g. 'TypeTags::Array' or
 * 'TypeTags::bsonArray'). The array is never owned by this slot. Provides an interface to iterate
 * over the values that constitute the array.
 *
 * It is illegal to fill out the slot with a type that is not array-like.
 */
class ArrayAccessor final : public SlotAccessor {
public:
    void reset(SlotAccessor* input) {
        _input = input;
        _currentIndex = 0;
        refresh();
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
        ++_currentIndex;
        return _enumerator.advance();
    }

    void refresh() {
        if (_input) {
            auto [tag, val] = _input->getViewOfValue();
            _enumerator.reset(tag, val, _currentIndex);
        }
    }

private:
    size_t _currentIndex = 0;
    SlotAccessor* _input{nullptr};
    ArrayEnumerator _enumerator;
};  // class ArrayAccessor

/**
 * This is a switched accessor - it holds a vector of accessors and operates on an accessor selected
 * (switched) by the index field.
 */
class SwitchAccessor final : public SlotAccessor {
public:
    SwitchAccessor(std::vector<SlotAccessor*> accessors) : _accessors(std::move(accessors)) {
        invariant(!_accessors.empty());
    }

    std::pair<TypeTags, Value> getViewOfValue() const override {
        return _accessors[_index]->getViewOfValue();
    }
    std::pair<TypeTags, Value> copyOrMoveValue() override {
        return _accessors[_index]->copyOrMoveValue();
    }

    void setIndex(size_t index) {
        invariant(index < _accessors.size());
        _index = index;
    }

private:
    std::vector<SlotAccessor*> _accessors;
    size_t _index{0};
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
        return _it->first.getViewOfValue(_slot);
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
        return _it->second.getViewOfValue(_slot);
    }
    std::pair<TypeTags, Value> copyOrMoveValue() override {
        return _it->second.copyOrMoveValue(_slot);
    }

    void reset(bool owned, TypeTags tag, Value val) {
        _it->second.reset(_slot, owned, tag, val);
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
        return _container[_it].getViewOfValue(_slot);
    }
    std::pair<TypeTags, Value> copyOrMoveValue() override {
        return _container[_it].copyOrMoveValue(_slot);
    }

    void reset(bool owned, TypeTags tag, Value val) {
        _container[_it].reset(_slot, owned, tag, val);
    }

private:
    T& _container;
    const size_t& _it;
    const size_t _slot;
};

/**
 * Provides a view of a slot inside a single MaterializedRow.
 */
template <typename RowType>
class SingleRowAccessor final : public SlotAccessor {
public:
    /**
     * Constructs an accessor that gives a view of the value at the given 'slot' of a
     * given single row.
     */
    SingleRowAccessor(RowType& row, size_t slot) : _row(row), _slot(slot) {}

    std::pair<TypeTags, Value> getViewOfValue() const override {
        return _row.getViewOfValue(_slot);
    }
    std::pair<TypeTags, Value> copyOrMoveValue() override {
        return _row.copyOrMoveValue(_slot);
    }
    void reset(bool owned, TypeTags tag, Value val) {
        _row.reset(_slot, owned, tag, val);
    }

private:
    RowType& _row;
    const size_t _slot;
};
typedef SingleRowAccessor<MaterializedRow> MaterializedSingleRowAccessor;

/**
 * Read the components of the 'keyString' value and populate 'accessors' with those components. Some
 * components are appended into the 'valueBufferBuilder' object's internal buffer, and the accessors
 * populated with those values will hold pointers into the buffer. The 'valueBufferBuilder' is
 * caller owned, and it can be reset and reused once it is safe to invalidate any accessors that
 * might reference it.
 */
void readKeyStringValueIntoAccessors(
    const key_string::Value& keyString,
    const Ordering& ordering,
    BufBuilder* valueBufferBuilder,
    std::vector<OwnedValueAccessor>* accessors,
    boost::optional<IndexKeysInclusionSet> indexKeysToInclude = boost::none);


/**
 * Commonly used containers.
 */
template <typename T>
using SlotMap = absl::flat_hash_map<SlotId, T>;
using SlotAccessorMap = SlotMap<SlotAccessor*>;
using FieldAccessorMap = StringMap<std::unique_ptr<OwnedValueAccessor>>;
using FieldViewAccessorMap = StringMap<std::unique_ptr<ViewOfValueAccessor>>;
using SlotSet = absl::flat_hash_set<SlotId>;
using SlotVector = absl::InlinedVector<SlotId, 2>;

using SlotIdGenerator = IdGenerator<value::SlotId, SlotVector>;
using FrameIdGenerator = IdGenerator<FrameId>;
using SpoolIdGenerator = IdGenerator<SpoolId>;

/**
 * Given an unordered slot 'map', calls 'callback' for each slot/value pair in order of ascending
 * slot id.
 */
template <typename T, typename C>
void orderedSlotMapTraverse(const SlotMap<T>& map, C callback) {
    std::set<SlotId> slots;
    for (auto&& elem : map) {
        slots.insert(elem.first);
    }

    for (auto slot : slots) {
        callback(slot, map.at(slot));
    }
}


/**
 * Accessor for a slot which can own the value held by that slot and provides optimized BSONObj
 * access.
 */
class BSONObjValueAccessor final : public SlotAccessor {
public:
    BSONObjValueAccessor() = default;

    BSONObjValueAccessor(const BSONObjValueAccessor& other) {
        if (other._owned) {
            auto [tag, val] = copyValue(other._tag, other._val);
            _tag = tag;
            _val = val;
            _owned = true;
        } else {
            _tag = other._tag;
            _val = other._val;
            _owned = false;
            _hasBsonObj = other._hasBsonObj;
            _bsonObj = other._bsonObj;
        }
    }

    BSONObjValueAccessor(BSONObjValueAccessor&& other) noexcept {
        _hasBsonObj = other._hasBsonObj;
        _tag = other._tag;
        _val = other._val;
        _owned = other._owned;
        _bsonObj = other._bsonObj;

        other._hasBsonObj = false;
        other._owned = false;
        other._bsonObj = BSONObj();
    }

    ~BSONObjValueAccessor() {
        release();
    }

    // Copy and swap idiom for a single copy/move assignment operator.
    BSONObjValueAccessor& operator=(BSONObjValueAccessor other) noexcept {
        std::swap(_hasBsonObj, other._hasBsonObj);
        std::swap(_tag, other._tag);
        std::swap(_val, other._val);
        std::swap(_owned, other._owned);
        std::swap(_bsonObj, other._bsonObj);
        return *this;
    }

    /**
     * Returns a non-owning view of the value.
     */
    std::pair<TypeTags, Value> getViewOfValue() const override {
        SlotAccessorHelper::dassertValidSlotValue(_tag, _val);
        return {_tag, _val};
    }

    /**
     * If a the value is owned by this slot, then the slot relinquishes ownership of the returned
     * value. Alternatively, if the value is unowned, then the caller receives a copy. Either way,
     * the caller owns the resulting value.
     */
    std::pair<TypeTags, Value> copyOrMoveValue() override {
        SlotAccessorHelper::dassertValidSlotValue(_tag, _val);
        if (_owned && !_hasBsonObj) {
            _owned = false;
            return {_tag, _val};
        } else {
            return copyValue(_tag, _val);
        }
    }

    BSONObj getOwnedBSONObj() {
        invariant(_tag == TypeTags::bsonObject);
        if (!_hasBsonObj) {
            if (!_owned) {
                std::tie(_tag, _val) = copyValue(_tag, _val);
            }

            auto sharedBuf =
                SharedBuffer(UniqueBuffer::reclaim(sbe::value::bitcastTo<char*>(_val)));
            _hasBsonObj = true;
            _owned = false;
            _bsonObj = BSONObj{std::move(sharedBuf)};
        }

        return _bsonObj;
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

    void makeOwned() {
        if (_owned || _hasBsonObj) {
            return;
        }

        std::tie(_tag, _val) = copyValue(_tag, _val);
        _owned = true;
    }

private:
    void release() {
        if (_owned) {
            releaseValue(_tag, _val);
            _owned = false;
        }
        _hasBsonObj = false;
        _bsonObj = BSONObj();
    }

    bool _hasBsonObj{false};
    bool _owned{false};
    TypeTags _tag{TypeTags::Nothing};
    Value _val{0};
    BSONObj _bsonObj;
};  // class BSONObjValueAccessor

}  // namespace mongo::sbe::value
