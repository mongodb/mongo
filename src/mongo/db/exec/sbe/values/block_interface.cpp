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
    for (size_t i = 0; i < deblocked.count(); ++i) {
        s << std::pair(deblocked.tags()[i], deblocked.vals()[i]) << " ";
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
    if ((op.opType.flags & ColumnOpType::kMonotonic) && tryDense().get_value_or(false) &&
        tryCount()) {
        auto [lbTag, lbVal] = tryLowerBound();
        auto [ubTag, ubVal] = tryUpperBound();

        if (lbTag == ubTag && lbTag != value::TypeTags::Nothing) {
            auto [lbResTag, lbResVal] = op.processSingle(lbTag, lbVal);
            ValueGuard minGuard(lbResTag, lbResVal);
            auto [ubResTag, ubResVal] = op.processSingle(ubTag, ubVal);
            ValueGuard maxGuard(ubResTag, ubResVal);

            auto [cmpTag, cmpVal] = value::compareValue(lbResTag, lbResVal, ubResTag, ubResVal);
            if (cmpTag == value::TypeTags::NumberInt32 && cmpVal == 0) {
                minGuard.reset();
                return std::make_unique<MonoBlock>(count(), lbResTag, lbResVal);
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

    if (extracted.count() == 0) {
        return std::make_unique<HeterogeneousBlock>();
    }

    std::vector<TypeTags> tags(extracted.count(), TypeTags::Nothing);
    std::vector<Value> vals(extracted.count(), Value{0u});

    ValueVectorGuard blockGuard(tags, vals);
    op.processBatch(
        extracted.tags(), extracted.vals(), tags.data(), vals.data(), extracted.count());
    blockGuard.reset();

    return buildBlockFromStorage(std::move(tags), std::move(vals));
}

std::unique_ptr<ValueBlock> ValueBlock::buildBlockFromStorage(std::vector<value::TypeTags> tags,
                                                              std::vector<value::Value> vals) {
    if (vals.empty()) {
        return std::make_unique<HeterogeneousBlock>();
    }

    if ((validHomogeneousType(tags[0]) || tags[0] == TypeTags::Nothing) &&
        std::all_of(tags.begin(), tags.end(), [&](TypeTags tag) { return tag == tags[0]; })) {
        switch (tags[0]) {
            case value::TypeTags::Nothing: {
                return std::make_unique<MonoBlock>(vals.size(), value::TypeTags::Nothing, 0);
            }
            case value::TypeTags::Boolean: {
                return std::make_unique<value::BoolBlock>(std::move(vals));
            }
            case value::TypeTags::NumberInt32: {
                return std::make_unique<value::Int32Block>(std::move(vals));
            }
            case value::TypeTags::NumberInt64: {
                return std::make_unique<value::Int64Block>(std::move(vals));
            }
            case value::TypeTags::NumberDouble: {
                return std::make_unique<value::DoubleBlock>(std::move(vals));
            }
            case value::TypeTags::Date: {
                return std::make_unique<value::DateBlock>(std::move(vals));
            }
            default:
                break;
        }
    }

    bool isDense = std::all_of(
        tags.begin(), tags.end(), [](TypeTags tag) { return tag != TypeTags::Nothing; });

    return std::make_unique<HeterogeneousBlock>(std::move(tags), std::move(vals), isDense);
}

std::unique_ptr<ValueBlock> HeterogeneousBlock::map(const ColumnOp& op) {
    if (auto fastPathResult = this->mapMonotonicFastPath(op); fastPathResult) {
        return fastPathResult;
    }

    size_t numElems = _vals.size();
    if (numElems == 0) {
        return std::make_unique<HeterogeneousBlock>();
    }

    std::vector<TypeTags> tags(numElems, TypeTags::Nothing);
    std::vector<Value> vals(numElems, Value{0u});

    ValueVectorGuard blockGuard(tags, vals);
    op.processBatch(_tags.data(), _vals.data(), tags.data(), vals.data(), numElems);
    blockGuard.reset();

    return buildBlockFromStorage(std::move(tags), std::move(vals));
}

TokenizedBlock ValueBlock::tokenize() {
    auto extracted = extract();
    std::vector<TypeTags> tokenTags;
    std::vector<Value> tokenVals;
    std::vector<size_t> idxs(extracted.count(), 0);

    size_t uniqueCount = 0;
    auto tokenMap = ValueMapType<size_t>{0, value::ValueHash(), value::ValueEq()};
    for (size_t i = 0; i < extracted.count(); ++i) {
        auto [it, inserted] =
            tokenMap.insert({extracted.tags()[i], extracted.vals()[i]}, uniqueCount);
        if (inserted) {
            uniqueCount++;
            auto [cpyTag, cpyVal] = value::copyValue(extracted.tags()[i], extracted.vals()[i]);
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

namespace {
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
}  // namespace

template <typename T, value::TypeTags TypeTag>
std::unique_ptr<ValueBlock> HomogeneousBlock<T, TypeTag>::map(const ColumnOp& op) {
    if (auto fastPathResult = this->mapMonotonicFastPath(op); fastPathResult) {
        return fastPathResult;
    }

    size_t numElems = _vals.size();
    if (numElems == 0) {
        return std::make_unique<HeterogeneousBlock>();
    }

    std::vector<TypeTags> tags(numElems, TypeTags::Nothing);
    std::vector<Value> vals(numElems, Value{0u});

    ValueVectorGuard blockGuard(tags, vals);
    // Fast path for dense case, everything can be processed in one chunk.
    op.processBatch(TypeTag, _vals.data(), tags.data(), vals.data(), numElems);

    if (_presentBitset.all()) {
        blockGuard.reset();
        return buildBlockFromStorage(std::move(tags), std::move(vals));
    }

    // If the block is not dense, we have to get the result for Nothing input(s).
    auto [nullTag, nullVal] = op.processSingle(value::TypeTags::Nothing, 0);
    ValueGuard nullGuard(nullTag, nullVal);

    // Then, insert it in the proper places.
    size_t blockSize = size();
    std::vector<TypeTags> mergedTags(blockSize, TypeTags::Nothing);
    std::vector<Value> mergedVals(blockSize, Value{0u});

    size_t valIdx = 0;
    for (size_t i = 0; i < _presentBitset.size(); ++i) {
        if (_presentBitset[i]) {
            mergedTags[i] = tags[valIdx];
            mergedVals[i] = vals[valIdx];
            valIdx++;
        } else {
            std::tie(mergedTags[i], mergedVals[i]) = sbe::value::copyValue(nullTag, nullVal);
        }
    }
    blockGuard.reset();
    return buildBlockFromStorage(std::move(mergedTags), std::move(mergedVals));
}

template std::unique_ptr<ValueBlock> BoolBlock::map(const ColumnOp& op);
template std::unique_ptr<ValueBlock> Int32Block::map(const ColumnOp& op);
template std::unique_ptr<ValueBlock> Int64Block::map(const ColumnOp& op);
template std::unique_ptr<ValueBlock> DateBlock::map(const ColumnOp& op);
template std::unique_ptr<ValueBlock> DoubleBlock::map(const ColumnOp& op);


// Should not be used for DoubleBlocks since hashValue has special handling of NaN's that differs
// from naively using absl::Hash<double>.
template <typename T, value::TypeTags TypeTag>
TokenizedBlock HomogeneousBlock<T, TypeTag>::tokenize() {
    std::vector<Value> tokenVals{};
    std::vector<size_t> idxs(_presentBitset.size(), 0);

    auto tokenMap = absl::flat_hash_map<Value, size_t, absl::Hash<T>, IntValueEq<T>>{};

    bool isDense = *tryDense();
    size_t uniqueCount = 0;
    if (!isDense) {
        tokenVals.push_back(Value{0u});
        ++uniqueCount;
    }

    // We make Nothing the first token and initialize 'idxs' to all zeroes. This means that Nothing
    // is our "default" value, and we only have to set values in idxes for non-Nothings.
    size_t bitsetIndex = _presentBitset.find_first();
    for (size_t i = 0; i < _vals.size() && bitsetIndex < _presentBitset.size(); ++i) {
        auto [it, inserted] = tokenMap.insert({_vals[i], uniqueCount});
        if (inserted) {
            ++uniqueCount;
            tokenVals.push_back(_vals[i]);
        }
        idxs[bitsetIndex] = it->second;
        bitsetIndex = _presentBitset.find_next(bitsetIndex);
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

std::unique_ptr<ValueBlock> ValueBlock::fillEmpty(TypeTags fillTag, Value fillVal) {
    if (tryDense().get_value_or(false)) {
        return nullptr;
    }

    auto deblocked = extract();
    std::vector<TypeTags> tags(deblocked.count());
    std::vector<Value> vals(deblocked.count());
    for (size_t i = 0; i < deblocked.count(); ++i) {
        if (deblocked.tags()[i] == value::TypeTags::Nothing) {
            auto [tag, val] = value::copyValue(fillTag, fillVal);
            tags[i] = tag;
            vals[i] = val;
        } else {
            auto [tag, val] = value::copyValue(deblocked.tags()[i], deblocked.vals()[i]);
            tags[i] = tag;
            vals[i] = val;
        }
    }

    return std::make_unique<HeterogeneousBlock>(
        std::move(tags), std::move(vals), true /* isDense */);
}

template <typename T, value::TypeTags TypeTag>
std::unique_ptr<ValueBlock> HomogeneousBlock<T, TypeTag>::fillEmpty(TypeTags fillTag,
                                                                    Value fillVal) {
    if (*tryDense()) {
        return nullptr;
    } else if (fillTag == TypeTag) {
        if (_presentBitset.none()) {
            return std::make_unique<MonoBlock>(_presentBitset.size(), fillTag, fillVal);
        }
        // We also know that fillTag must be shallow since HomogeneousBlocks can only store
        // shallow types.
        size_t valsIndex = 0;
        std::vector<Value> vals(_presentBitset.size());
        for (size_t i = 0; i < _presentBitset.size(); ++i) {
            if (_presentBitset[i]) {
                vals[i] = _vals[valsIndex++];
            } else {
                vals[i] = fillVal;
            }
        }
        return std::make_unique<HomogeneousBlock<T, TypeTag>>(std::move(vals));
    }
    return ValueBlock::fillEmpty(fillTag, fillVal);
}

template std::unique_ptr<ValueBlock> Int32Block::fillEmpty(TypeTags fillTag, Value fillVal);
template std::unique_ptr<ValueBlock> Int64Block::fillEmpty(TypeTags fillTag, Value fillVal);
template std::unique_ptr<ValueBlock> DateBlock::fillEmpty(TypeTags fillTag, Value fillVal);
template std::unique_ptr<ValueBlock> DoubleBlock::fillEmpty(TypeTags fillTag, Value fillVal);
template std::unique_ptr<ValueBlock> BoolBlock::fillEmpty(TypeTags fillTag, Value fillVal);

std::unique_ptr<ValueBlock> ValueBlock::exists() {
    if (tryDense().get_value_or(false) && tryCount()) {
        return std::make_unique<MonoBlock>(
            *tryCount(), TypeTags::Boolean, value::bitcastFrom<bool>(true));
    }
    auto extracted = extract();
    std::vector<Value> vals(extracted.count());
    for (size_t i = 0; i < extracted.count(); ++i) {
        vals[i] = value::bitcastFrom<bool>(extracted.tags()[i] != TypeTags::Nothing);
    }
    return std::make_unique<BoolBlock>(std::move(vals));
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
