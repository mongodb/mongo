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
struct DeblockedTagVals {
    // 'tags' and 'vals' point to an array of 'count' elements respectively.
    DeblockedTagVals(size_t count, const TypeTags* tags, const Value* vals)
        : count(count), tags(tags), vals(vals) {
        tassert(7949501, "Values must exist", count > 0 && tags != nullptr && vals != nullptr);
    }

    std::pair<TypeTags, Value> operator[](size_t idx) const {
        return {tags[idx], vals[idx]};
    }

    const size_t count;
    const TypeTags* const tags;
    const Value* const vals;
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

    virtual std::unique_ptr<ValueBlock> map(const ColumnOp& op) const;

protected:
    virtual DeblockedTagVals deblock(boost::optional<DeblockedTagValStorage>& storage) const = 0;

    std::unique_ptr<ValueBlock> defaultMapImpl(const ColumnOp& op) const;

    boost::optional<DeblockedTagValStorage> _deblockedStorage;
};

/**
 * A block that is a run of repeated values.
 */
class MonoBlock final : public ValueBlock {
public:
    MonoBlock(size_t count, TypeTags tag, Value val) : _count(count) {
        tassert(7962102, "The number of values must be > 0", count > 0);
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

    DeblockedTagVals deblock(boost::optional<DeblockedTagValStorage>& storage) const override {
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

    std::unique_ptr<ValueBlock> map(const ColumnOp& op) const override {
        auto [tag, val] = op.processSingle(_tag, _val);
        return std::make_unique<MonoBlock>(_count, tag, val);
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

    HeterogeneousBlock(const HeterogeneousBlock& o) : ValueBlock(o) {
        _vals.resize(o._vals.size(), Value{0u});
        _tags.resize(o._tags.size(), TypeTags::Nothing);

        for (size_t i = 0; i < o._vals.size(); ++i) {
            auto [copyTag, copyVal] = copyValue(o._tags[i], o._vals[i]);
            _vals[i] = copyVal;
            _tags[i] = copyTag;
        }
    }

    HeterogeneousBlock(HeterogeneousBlock&& o)
        : ValueBlock(std::move(o)), _vals(std::move(o._vals)), _tags(std::move(o._tags)) {
        o._vals = {};
        o._tags = {};
    }

    HeterogeneousBlock(std::vector<TypeTags> tags, std::vector<Value> vals)
        : _vals(std::move(vals)), _tags(std::move(tags)) {}

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

    DeblockedTagVals deblock(boost::optional<DeblockedTagValStorage>& storage) const override {
        return {_vals.size(), _tags.data(), _vals.data()};
    }

    std::unique_ptr<ValueBlock> clone() const override {
        return std::make_unique<HeterogeneousBlock>(*this);
    }

    std::unique_ptr<ValueBlock> map(const ColumnOp& op) const override;

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
};
}  // namespace mongo::sbe::value
