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

#include <boost/dynamic_bitset/dynamic_bitset.hpp>
#include <boost/optional/optional.hpp>
#include <cstddef>
#include <memory>

#include "mongo/db/exec/sbe/values/cell_interface.h"
#include "mongo/db/exec/sbe/values/column_op.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/util/assert_util.h"

namespace mongo::sbe::value {
/**
 * Deblocked tags and values for a ValueBlock.
 *
 * Note: Deblocked values are read-only and must not be modified.
 */
class DeblockedTagVals {
public:
    DeblockedTagVals() {}

    // 'tags' and 'vals' point to an array of 'count' elements respectively.
    DeblockedTagVals(size_t count, const TypeTags* tags, const Value* vals)
        : _count(count), _tags(tags), _vals(vals) {
        tassert(7949501, "Values must exist", count > 0 || (tags == nullptr && vals == nullptr));
    }
    DeblockedTagVals& operator=(const DeblockedTagVals& other) {
        _count = other._count;
        _tags = other._tags;
        _vals = other._vals;
        return *this;
    }


    std::pair<TypeTags, Value> operator[](size_t idx) const {
        return {_tags[idx], _vals[idx]};
    }

    std::span<const TypeTags> tagsSpan() const {
        return std::span(_tags, _count);
    }

    std::span<const Value> valsSpan() const {
        return std::span(_vals, _count);
    }

    size_t count() const {
        return _count;
    }

    const TypeTags* tags() const {
        return _tags;
    }

    const Value* vals() const {
        return _vals;
    }

private:
    size_t _count = 0;
    const TypeTags* _tags = nullptr;
    const Value* _vals = nullptr;
};

// Bitset representation used to indicate present or missing values. DynamicBitset from
// mongo/util/dynamic_bitset.h does not store the bit size of the bitset and thus is missing all(),
// push_back() and the size() behavior of boost::dynamic_bitset that we need to implement
// homogeneous blocks.
using HomogeneousBlockBitset = boost::dynamic_bitset<size_t>;

/**
 * Tokens representing unique values in a block and indexes that represent the location of these
 * values in the original block.
 * 'idxs' maps index in the original block to index in tokens.
 */
struct TokenizedBlock {
    std::unique_ptr<ValueBlock> tokens;
    std::vector<size_t> idxs;
};

std::ostream& operator<<(std::ostream& s, const DeblockedTagVals& deblocked);
str::stream& operator<<(str::stream& str, const DeblockedTagVals& deblocked);

struct DeblockedTagValStorage {
    DeblockedTagValStorage() = default;

    DeblockedTagValStorage(const DeblockedTagValStorage& other) {
        copyValuesFrom(other);
    }

    DeblockedTagValStorage(DeblockedTagValStorage&& other)
        : tags(std::move(other.tags)), vals(std::move(other.vals)), owned(other.owned) {
        other.tags = {};
        other.vals = {};
        other.owned = false;
    }

    ~DeblockedTagValStorage() {
        release();
    }

    DeblockedTagValStorage& operator=(const DeblockedTagValStorage& other) {
        if (this != &other) {
            release();

            tags.clear();
            vals.clear();
            copyValuesFrom(other);
        }
        return *this;
    }

    DeblockedTagValStorage& operator=(DeblockedTagValStorage&& other) {
        if (this != &other) {
            release();

            tags = std::move(other.tags);
            vals = std::move(other.vals);
            owned = other.owned;

            other.tags = {};
            other.vals = {};
            other.owned = false;
        }
        return *this;
    }

    void copyValuesFrom(const DeblockedTagValStorage& other);

    void release();

    std::vector<TypeTags> tags;
    std::vector<Value> vals;
    bool owned{false};
};

/**
 * Interface for accessing a sequence of SBE Values independent of their backing storage.
 *
 * Currently we only support getting all of the deblocked values via 'extract()' but PM-3168 will
 * extend the interface to allow for other operations to be applied which may run directly on the
 * underlying format or take advantage of precomputed summaries.
 *
 * In general no functions on a ValueBlock should be considered thread-safe, regardless of
 * constness.
 */
struct ValueBlock {
    ValueBlock() = default;

    // When copy-constructing a ValueBlock, if o's deblocked storage is owned then we will copy it.
    // If o's deblocked storage is not owned, then it's not permissible for this block to copy it
    // (because the deblocked storage contains views of SBE values whose lifetimes are not managed
    // this ValueBlock), so we ignore it.
    ValueBlock(const ValueBlock& o)
        : _deblockedStorage(o._deblockedStorage && o._deblockedStorage->owned ? o._deblockedStorage
                                                                              : boost::none) {}

    ValueBlock(ValueBlock&&) = default;

    virtual ~ValueBlock() = default;

    ValueBlock& operator=(const ValueBlock&) = delete;
    ValueBlock& operator=(ValueBlock&&) = delete;

    /**
     * Returns the unowned deblocked values. The return value is only valid as long as the block
     * remains alive. The returned values must be dense, meaning that there are always same
     * number of values as the count() of this block. The 'DeblockedTagVals.count' must always be
     * equal to this block's count().
     */
    DeblockedTagVals extract() {
        return deblock(_deblockedStorage);
    }

    /**
     * Returns a copy of this block.
     */
    virtual std::unique_ptr<ValueBlock> clone() const = 0;

    /**
     * Returns the number of values in this block in O(1) time, otherwise returns boost::none.
     */
    virtual boost::optional<size_t> tryCount() const = 0;

    /**
     * Returns the number of values in this block. If this cannot be done in O(1) then the values
     * will be extracted and the count will be returned.
     */
    size_t count() {
        auto elsNum = tryCount();
        if (elsNum.has_value()) {
            return elsNum.get();
        }

        return this->extract().count();
    }

    /**
     * Returns the minimum value in the block in O(1) time, otherwise returns Nothing value.
     */
    virtual std::pair<TypeTags, Value> tryMin() const {
        return std::pair(TypeTags::Nothing, Value{0u});
    }

    /**
     * Returns the maximum value in the block in O(1) time, otherwise returns Nothing value.
     */
    virtual std::pair<TypeTags, Value> tryMax() const {
        return std::pair(TypeTags::Nothing, Value{0u});
    }

    /**
     * Returns true if every value in the block is guaranteed to be non-nothing, false otherwise. If
     * this can't be determined in O(1) time, return boost::none.
     */
    virtual boost::optional<bool> tryDense() const {
        return boost::none;
    }

    /**
     * Allows the caller to cast this to a specific block type. Should only be used for SBE-native
     * block types (not types representing external storage).
     */
    template <typename T>
    T* as() {
        return dynamic_cast<T*>(this);
    }

    virtual std::unique_ptr<ValueBlock> map(const ColumnOp& op);

    virtual TokenizedBlock tokenize();

    /**
     * Returns a block where all Nothings are replaced with (fillTag, fillVal) or nullptr if the
     * block was already dense.
     */
    virtual std::unique_ptr<ValueBlock> fillEmpty(TypeTags fillTag, Value fillVal);

    /**
     * Returns a block of booleans, where non-Nothing values in the block are mapped to true, and
     * Nothings are mapped to false.
     */
    virtual std::unique_ptr<ValueBlock> exists();

    std::unique_ptr<ValueBlock> mapMonotonicFastPath(const ColumnOp& op);

    /**
     * If the block this is called on contains Nothings or other type tags that are not Boolean,
     * return boost::none. If this is called on a block that is strictly all Booleans, return
     * whether or not the values are all false/true. Any caller of this function that expects the
     * presence of non-Booleans to be an error should check for this after getting a return value of
     * boost::none.
     */
    virtual boost::optional<bool> allFalse() const {
        return boost::none;
    }

    virtual boost::optional<bool> allTrue() const {
        return boost::none;
    }

protected:
    virtual DeblockedTagVals deblock(boost::optional<DeblockedTagValStorage>& storage) = 0;

    std::unique_ptr<ValueBlock> defaultMapImpl(const ColumnOp& op);

    std::unique_ptr<ValueBlock> buildBlockFromStorage(std::vector<value::TypeTags> tags,
                                                      std::vector<value::Value> vals);

    boost::optional<DeblockedTagValStorage> _deblockedStorage;
};

// TODO SERVER-83799 Remove TypeTags::Boolean
inline constexpr bool validHomogeneousType(TypeTags tag) {
    return tag == TypeTags::NumberInt32 || tag == TypeTags::NumberInt64 || tag == TypeTags::Date ||
        tag == TypeTags::NumberDouble || tag == TypeTags::Boolean;
}

/**
 * A block that is a run of repeated values.
 */
class MonoBlock final : public ValueBlock {
public:
    MonoBlock(size_t count, TypeTags tag, Value val) : _count(count) {
        std::tie(_tag, _val) = copyValue(tag, val);
    }

    MonoBlock(const MonoBlock& o) : ValueBlock(o), _count(o._count) {
        std::tie(_tag, _val) = copyValue(o._tag, o._val);
    }

    MonoBlock(MonoBlock&& o)
        : ValueBlock(std::move(o)), _tag(o._tag), _val(o._val), _count(o._count) {
        o._tag = TypeTags::Nothing;
        o._val = 0;
    }

    ~MonoBlock() {
        releaseValue(_tag, _val);
    }

    std::unique_ptr<ValueBlock> clone() const override {
        return std::make_unique<MonoBlock>(*this);
    }

    DeblockedTagVals deblock(boost::optional<DeblockedTagValStorage>& storage) override {
        if (!storage) {
            storage = DeblockedTagValStorage{};
        }

        if (storage->tags.size() != _count) {
            storage->tags.clear();
            storage->vals.clear();
            storage->tags.resize(_count, _tag);
            storage->vals.resize(_count, _val);
        }

        return {_count, storage->tags.data(), storage->vals.data()};
    }

    boost::optional<size_t> tryCount() const override {
        return _count;
    }

    std::pair<TypeTags, Value> tryMin() const override {
        return std::pair(_tag, _val);
    }

    std::pair<TypeTags, Value> tryMax() const override {
        return std::pair(_tag, _val);
    }

    boost::optional<bool> tryDense() const override {
        return _tag != TypeTags::Nothing;
    }

    std::unique_ptr<ValueBlock> map(const ColumnOp& op) override {
        auto [tag, val] = op.processSingle(_tag, _val);
        return std::make_unique<MonoBlock>(_count, tag, val);
    }

    TokenizedBlock tokenize() override;

    std::unique_ptr<ValueBlock> fillEmpty(TypeTags fillTag, Value fillVal) override {
        if (*tryDense()) {
            return nullptr;
        }
        return std::make_unique<MonoBlock>(_count, fillTag, fillVal);
    }

    std::unique_ptr<ValueBlock> exists() override {
        return std::make_unique<MonoBlock>(
            _count, TypeTags::Boolean, value::bitcastFrom<bool>(*tryDense()));
    }

    TypeTags getTag() const {
        return _tag;
    }

    Value getValue() const {
        return _val;
    }

    boost::optional<bool> allFalse() const override {
        if (_tag != TypeTags::Boolean) {
            return boost::none;
        }
        return !value::bitcastTo<bool>(_val);
    }

    boost::optional<bool> allTrue() const override {
        if (_tag != TypeTags::Boolean) {
            return boost::none;
        }
        return value::bitcastTo<bool>(_val);
    }

private:
    // Always owned.
    TypeTags _tag;
    Value _val;

    // To lazily extract the values, we need to remember the number of values which is supposed
    // to exist in this block.
    size_t _count;
};

class HeterogeneousBlock : public ValueBlock {
public:
    HeterogeneousBlock() = default;

    HeterogeneousBlock(const HeterogeneousBlock& o) : ValueBlock(o), _isDense(o._isDense) {
        _vals.resize(o._vals.size(), Value{0u});
        _tags.resize(o._tags.size(), TypeTags::Nothing);

        for (size_t i = 0; i < o._vals.size(); ++i) {
            auto [copyTag, copyVal] = copyValue(o._tags[i], o._vals[i]);
            _vals[i] = copyVal;
            _tags[i] = copyTag;
        }
    }

    HeterogeneousBlock(HeterogeneousBlock&& o)
        : ValueBlock(std::move(o)),
          _vals(std::move(o._vals)),
          _tags(std::move(o._tags)),
          _isDense(o._isDense) {
        o._vals = {};
        o._tags = {};
    }

    HeterogeneousBlock(std::vector<TypeTags> tags, std::vector<Value> vals, bool isDense = false)
        : _vals(std::move(vals)), _tags(std::move(tags)), _isDense(isDense) {}

    ~HeterogeneousBlock() {
        release();
    }

    void clear() noexcept {
        release();
        _tags.clear();
        _vals.clear();
    }

    void reserve(size_t n) {
        _vals.reserve(n);
        _tags.reserve(n);
    }

    size_t size() const {
        return _tags.size();
    }

    void push_back(TypeTags t, Value v);

    void push_back(std::pair<TypeTags, Value> tv) {
        push_back(tv.first, tv.second);
    }

    boost::optional<size_t> tryCount() const override {
        return _vals.size();
    }

    std::pair<TypeTags, Value> tryMin() const override {
        return {TypeTags::Nothing, Value{0u}};
    }

    std::pair<TypeTags, Value> tryMax() const override {
        return {TypeTags::Nothing, Value{0u}};
    }

    boost::optional<bool> tryDense() const override {
        return _isDense;
    }

    DeblockedTagVals deblock(boost::optional<DeblockedTagValStorage>& storage) override {
        return {_vals.size(), _tags.data(), _vals.data()};
    }

    std::unique_ptr<ValueBlock> clone() const override {
        return std::make_unique<HeterogeneousBlock>(*this);
    }

    std::unique_ptr<ValueBlock> map(const ColumnOp& op) override;

private:
    void release() noexcept {
        invariant(_tags.size() == _vals.size());
        for (size_t i = 0; i < _vals.size(); ++i) {
            releaseValue(_tags[i], _vals[i]);
        }
    }

    // All values are owned.
    std::vector<Value> _vals;
    std::vector<TypeTags> _tags;

    // True if all values are non-nothing.
    bool _isDense = false;
};

template <typename T, value::TypeTags TypeTag>
class HomogeneousBlock : public ValueBlock {
public:
    HomogeneousBlock() = default;
    // HomogeneousBlock's can only store shallow values so we don't need to call copyValue on each
    // Value in o._vals.
    HomogeneousBlock(const HomogeneousBlock& o)
        : ValueBlock(o), _vals(o._vals), _presentBitset(o._presentBitset) {}

    HomogeneousBlock(HomogeneousBlock&& o)
        : ValueBlock(std::move(o)),
          _vals(std::move(o._vals)),
          _presentBitset(std::move(o._presentBitset)) {
        o._vals = {};
        o._presentBitset = {};
    }

    // TODO SERVER-83799 Remove this constructor. Should only be used to create BoolBlocks.
    HomogeneousBlock(std::vector<bool> input) {
        if constexpr (TypeTag == TypeTags::Boolean) {
            _vals.resize(input.size());
            for (size_t i = 0; i < input.size(); ++i) {
                _vals[i] = value::bitcastFrom<bool>(input[i]);
            }
            _presentBitset.resize(_vals.size(), true);
        } else {
            // The !std::is_same<T,T> is always false and will trigger a compile failure if this
            // branch is taken. If this branch is not taken, it will get discarded.
            static_assert(!std::is_same<T, T>::value, "Not supported for deep types");
        }
    }

    // TODO SERVER-83799 Remove this constructor. Should only be used to create BoolBlocks.
    HomogeneousBlock(std::vector<bool> input, HomogeneousBlockBitset bitset) {
        if constexpr (TypeTag == TypeTags::Boolean) {
            _vals.resize(input.size());
            for (size_t i = 0; i < input.size(); ++i) {
                _vals[i] = value::bitcastFrom<bool>(input[i]);
            }
            _presentBitset = bitset;
        } else {
            // The !std::is_same<T,T> is always false and will trigger a compile failure if this
            // branch is taken. If this branch is not taken, it will get discarded.
            static_assert(!std::is_same<T, T>::value,
                          "Constructor is only supported for BoolBlocks");
        }
    }

    HomogeneousBlock(std::vector<Value> input) : _vals(std::move(input)) {
        if constexpr (validHomogeneousType(TypeTag)) {
            _presentBitset.resize(_vals.size(), true);
        } else {
            // The !std::is_same<T,T> is always false and will trigger a compile failure if this
            // branch is taken. If this branch is not taken, it will get discarded.
            static_assert(!std::is_same<T, T>::value, "Not supported for deep types");
        }
    }

    HomogeneousBlock(std::vector<Value> input, HomogeneousBlockBitset bitset) {
        if constexpr (validHomogeneousType(TypeTag)) {
            _vals.resize(input.size());
            for (size_t i = 0; i < input.size(); ++i) {
                _vals[i] = input[i];
            }
            _presentBitset = bitset;
        } else {
            // The !std::is_same<T,T> is always false and will trigger a compile failure if this
            // branch is taken. If this branch is not taken, it will get discarded.
            static_assert(!std::is_same<T, T>::value, "Not supported for deep types");
        }
    }

    void clear() noexcept {
        _vals.clear();
        _presentBitset.clear();
    }

    void reserve(size_t n) {
        _vals.reserve(n);
        _presentBitset.reserve(n);
    }

    size_t size() const {
        return _presentBitset.size();
    }

    void push_back(T v) {
        _vals.push_back(value::bitcastFrom<T>(v));
        _presentBitset.push_back(true);
    }

    void push_back(Value v) {
        _vals.push_back(v);
        _presentBitset.push_back(true);
    }

    void pushNothing() {
        _presentBitset.push_back(false);
    }

    boost::optional<size_t> tryCount() const override {
        return size();
    }

    boost::optional<bool> tryDense() const override {
        return _presentBitset.all();
    }

    // getVector should be used in favor of this function if possible.
    DeblockedTagVals deblock(boost::optional<DeblockedTagValStorage>& storage) override {
        if (!storage) {
            storage = DeblockedTagValStorage{};
        }

        // Fast path for dense case.
        if (_presentBitset.all()) {
            storage->vals.resize(_vals.size());
            for (size_t i = 0; i < _presentBitset.size(); ++i) {
                storage->vals[i] = _vals[i];
            }
            storage->tags.resize(_vals.size(), TypeTag);
            return {storage->tags.size(), storage->tags.data(), storage->vals.data()};
        }

        storage->vals.resize(_presentBitset.size());
        storage->tags.resize(_presentBitset.size());
        size_t valIdx = 0;
        for (size_t i = 0; i < _presentBitset.size(); ++i) {
            if (_presentBitset[i]) {
                storage->vals[i] = _vals[valIdx];
                storage->tags[i] = TypeTag;
                valIdx++;
            } else {
                storage->vals[i] = 0u;
                storage->tags[i] = TypeTags::Nothing;
            }
        }

        return {storage->tags.size(), storage->tags.data(), storage->vals.data()};
    }

    std::unique_ptr<ValueBlock> clone() const override {
        return std::make_unique<HomogeneousBlock>(*this);
    }

    std::unique_ptr<ValueBlock> map(const ColumnOp& op) override;

    TokenizedBlock tokenize() override;

    std::unique_ptr<ValueBlock> fillEmpty(TypeTags fillTag, Value fillVal) override;

    std::unique_ptr<ValueBlock> exists() override {
        if (_presentBitset.all()) {
            return std::make_unique<MonoBlock>(
                _presentBitset.size(), TypeTags::Boolean, value::bitcastFrom<bool>(true));
        } else if (_presentBitset.none()) {
            return std::make_unique<MonoBlock>(
                _presentBitset.size(), TypeTags::Boolean, value::bitcastFrom<bool>(false));
        }
        // This does a copy and could be optimized but for now this doesn't matter.
        std::vector<Value> vals(_presentBitset.size());
        for (size_t i = 0; i < _presentBitset.size(); ++i) {
            vals[i] = value::bitcastFrom<bool>(_presentBitset[i]);
        }

        // TODO SERVER-83799 Return a BitsetBlock.
        return std::make_unique<HomogeneousBlock<bool, TypeTags::Boolean>>(std::move(vals));
    }

    boost::optional<bool> allFalse() const override {
        if (TypeTag != TypeTags::Boolean || !_presentBitset.all()) {
            return boost::none;
        }
        for (size_t i = 0; i < _vals.size(); ++i) {
            if (value::bitcastTo<bool>(_vals[i])) {
                return false;
            }
        }
        return true;
    }

    boost::optional<bool> allTrue() const override {
        if (TypeTag != TypeTags::Boolean || !_presentBitset.all()) {
            return boost::none;
        }
        for (size_t i = 0; i < _vals.size(); ++i) {
            if (!value::bitcastTo<bool>(_vals[i])) {
                return false;
            }
        }
        return true;
    }

    const std::vector<Value>& getVector() const {
        return _vals;
    }

private:
    // Present values are stored contiguously and missing values are stored in a separate
    // bitset, with 1 indicating present and 0 indicating missing.
    std::vector<Value> _vals;
    HomogeneousBlockBitset _presentBitset;
};

// TODO SERVER-83799 Remove BoolBlock.
using BoolBlock = HomogeneousBlock<bool, TypeTags::Boolean>;
using Int32Block = HomogeneousBlock<int32_t, TypeTags::NumberInt32>;
using Int64Block = HomogeneousBlock<int64_t, TypeTags::NumberInt64>;
using DateBlock = HomogeneousBlock<int64_t, TypeTags::Date>;
using DoubleBlock = HomogeneousBlock<double, TypeTags::NumberDouble>;
}  // namespace mongo::sbe::value
