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

#include "mongo/db/exec/sbe/values/block_interface.h"

namespace mongo::sbe::value {

template <class Stream>
Stream& streamInsertionImpl(Stream& s, const DeblockedTagVals& deblocked) {
    for (size_t i = 0; i < deblocked.count; ++i) {
        s << std::pair(deblocked.tags[i], deblocked.vals[i]) << " ";
    }
    return s;
}

std::ostream& operator<<(std::ostream& stream, const DeblockedTagVals& vals) {
    return streamInsertionImpl(stream, vals);
}

str::stream& operator<<(str::stream& stream, const DeblockedTagVals& vals) {
    return streamInsertionImpl(stream, vals);
}

void DeblockedTagValStorage::copyValuesFrom(const DeblockedTagValStorage& other) {
    if (other.owned) {
        owned = true;
        tags.resize(other.tags.size(), value::TypeTags::Nothing);
        vals.resize(other.vals.size(), 0);

        for (size_t i = 0; i < other.tags.size(); ++i) {
            auto [cpyTag, cpyVal] = copyValue(other.tags[i], other.vals[i]);
            tags[i] = cpyTag;
            vals[i] = cpyVal;
        }
    } else {
        owned = false;
        tags = other.tags;
        vals = other.vals;
    }
}

void DeblockedTagValStorage::release() {
    if (owned) {
        owned = false;
        for (size_t i = 0; i < tags.size(); ++i) {
            releaseValue(tags[i], vals[i]);
        }
    }
}

std::unique_ptr<ValueBlock> ValueBlock::map(const ColumnOp& op) {
    return defaultMapImpl(op);
}

std::unique_ptr<ValueBlock> ValueBlock::mapMonotonicFastPath(const ColumnOp& op) {
    // If the ColumnOp function is monotonic and the block is dense, we can try to map the whole
    // bucket to a MonoBlock instead of mapping each value iteratively.
    if (tryDense().get_value_or(false) && tryCount() &&
        (op.opType.flags & ColumnOpType::kMonotonic)) {
        auto [minTag, minVal] = tryMin();
        auto [maxTag, maxVal] = tryMax();

        if (minTag == maxTag && minTag != value::TypeTags::Nothing) {
            auto [minResTag, minResVal] = op.processSingle(minTag, minVal);
            auto [maxResTag, maxResVal] = op.processSingle(maxTag, maxVal);
            if (minResTag == maxResTag && minResVal == maxResVal) {
                auto [cpyTag, cpyVal] = copyValue(minResTag, minResVal);
                return std::make_unique<MonoBlock>(*tryCount(), cpyTag, cpyVal);
            }
        }
    }
    // The min and max didn't exist or didn't map to the same value so we need to process the whole
    // block.
    return nullptr;
}

std::unique_ptr<ValueBlock> ValueBlock::defaultMapImpl(const ColumnOp& op) {
    if (auto fastPathResult = this->mapMonotonicFastPath(op); fastPathResult) {
        return fastPathResult;
    }

    auto extracted = extract();

    if (extracted.count == 0) {
        return std::make_unique<HeterogeneousBlock>();
    }

    std::vector<TypeTags> tags(extracted.count, TypeTags::Nothing);
    std::vector<Value> vals(extracted.count, Value{0u});

    op.processBatch(extracted.tags, extracted.vals, tags.data(), vals.data(), extracted.count);

    bool isDense = std::all_of(
        tags.begin(), tags.end(), [](TypeTags tag) { return tag != TypeTags::Nothing; });

    return std::make_unique<HeterogeneousBlock>(std::move(tags), std::move(vals), isDense);
}

TokenizedBlock ValueBlock::tokenize() {
    auto extracted = extract();
    std::vector<TypeTags> tokenTags;
    std::vector<Value> tokenVals;
    std::vector<size_t> idxs(extracted.count, 0);

    size_t uniqueCount = 0;
    auto tokenMap = ValueMapType<size_t>{0, value::ValueHash(), value::ValueEq()};
    for (size_t i = 0; i < extracted.count; ++i) {
        auto [it, inserted] = tokenMap.insert({extracted.tags[i], extracted.vals[i]}, uniqueCount);
        if (inserted) {
            uniqueCount++;
            auto [cpyTag, cpyVal] = value::copyValue(extracted.tags[i], extracted.vals[i]);
            tokenTags.push_back(cpyTag), tokenVals.push_back(cpyVal);
        }
        idxs[i] = it->second;
    }
    return {std::make_unique<HeterogeneousBlock>(std::move(tokenTags), std::move(tokenVals)), idxs};
}

TokenizedBlock MonoBlock::tokenize() {
    auto [tag, val] = value::copyValue(_tag, _val);
    std::vector<TypeTags> tokenTags{tag};
    std::vector<Value> tokenVals{val};

    return {std::make_unique<HeterogeneousBlock>(std::move(tokenTags), std::move(tokenVals)),
            std::vector<size_t>(_count, 0)};
}

/**
 * Defines equivalence of two Value's. Should only be used for NumberInt32, NumberInt64, and Date.
 */
template <typename T>
struct IntValueEq {
    explicit IntValueEq() {}

    bool operator()(const Value& lhs, const Value& rhs) const {
        T bcLHS = bitcastTo<T>(lhs);
        T bcRHS = bitcastTo<T>(rhs);

        // flat_hash_map does not use three way comparsion, so this equality comparison is
        // sufficient.
        return bcLHS == bcRHS;
    }
};

// Should not be used for DoubleBlocks since hashValue has special handling of NaN's that differs
// from naively using absl::Hash<double>.
template <class T, value::TypeTags TypeTag>
TokenizedBlock HomogeneousBlock<T, TypeTag>::tokenize() {
    std::vector<Value> tokenVals{};
    std::vector<size_t> idxs(_missingBitset.size(), 0);

    auto tokenMap = absl::flat_hash_map<Value, size_t, absl::Hash<T>, IntValueEq<T>>{};

    bool isDense = *tryDense();
    size_t uniqueCount = 0;
    if (!isDense) {
        tokenVals.push_back(Value{0u});
        ++uniqueCount;
    }

    // We make Nothing the first token and initialize 'idxs' to all zeroes. This means that Nothing
    // is our "default" value, and we only have to set values in idxes for non-Nothings.
    size_t bitsetIndex = _missingBitset.find_first();
    for (size_t i = 0; i < _vals.size() && bitsetIndex < _missingBitset.size(); ++i) {
        auto [it, inserted] = tokenMap.insert({_vals[i], uniqueCount});
        if (inserted) {
            ++uniqueCount;
            tokenVals.push_back(_vals[i]);
        }
        idxs[bitsetIndex] = it->second;
        bitsetIndex = _missingBitset.find_next(bitsetIndex);
    }

    std::vector<TypeTags> tokenTags(uniqueCount, TypeTag);
    if (!isDense) {
        // First token is always Nothing for non-dense blocks.
        tokenTags[0] = TypeTags::Nothing;
    }
    return {std::make_unique<HeterogeneousBlock>(std::move(tokenTags), std::move(tokenVals)), idxs};
}

template TokenizedBlock Int32Block::tokenize();
template TokenizedBlock Int64Block::tokenize();
template TokenizedBlock DateBlock::tokenize();

template <>
TokenizedBlock DoubleBlock::tokenize() {
    return ValueBlock::tokenize();
}

template <>
TokenizedBlock BoolBlock::tokenize() {
    return ValueBlock::tokenize();
}

std::unique_ptr<ValueBlock> HeterogeneousBlock::map(const ColumnOp& op) {
    auto outBlock = std::make_unique<HeterogeneousBlock>();

    size_t numElems = _vals.size();

    if (numElems > 0) {
        const TypeTags* inTags = _tags.data();
        const Value* inVals = _vals.data();

        outBlock->_tags.resize(numElems, TypeTags::Nothing);
        outBlock->_vals.resize(numElems, Value{0u});

        TypeTags* outTags = outBlock->_tags.data();
        Value* outVals = outBlock->_vals.data();

        op.processBatch(inTags, inVals, outTags, outVals, numElems);
    }

    return outBlock;
}

void HeterogeneousBlock::push_back(TypeTags t, Value v) {
    constexpr auto maxSizeT = std::numeric_limits<size_t>::max();

    ValueGuard guard(t, v);

    size_t cap = std::min<size_t>(_vals.capacity(), _tags.capacity());
    if (_vals.size() == cap) {
        size_t newCap = cap < maxSizeT / 2 ? (cap ? cap * 2 : 1) : maxSizeT;
        newCap = std::max<size_t>(newCap, cap + 1);

        _vals.reserve(newCap);
        _tags.reserve(newCap);
    }

    _vals.push_back(v);
    _tags.push_back(t);

    guard.reset();
}

}  // namespace mongo::sbe::value
