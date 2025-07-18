/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/query/compiler/stats/rand_utils.h"

#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <tuple>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <boost/optional/optional.hpp>

namespace mongo::stats {
namespace value = sbe::value;

const std::string DatasetDescriptor::_alphabet =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

DatasetDescriptor::DatasetDescriptor(const DataTypeDistribution& dataTypeDistribution,
                                     size_t intNDV,
                                     int minInt,
                                     int maxInt,
                                     size_t strNDV,
                                     size_t minStrLen,
                                     size_t maxStrLen,
                                     std::shared_ptr<DatasetDescriptor> nestedDataDescriptor,
                                     double reuseScalarsRatio,
                                     size_t arrNDV,
                                     size_t minArrLen,
                                     size_t maxArrLen)
    : _gen{42},
      _reuseScalarsRatio(reuseScalarsRatio),
      _intNDV(std::min(intNDV, static_cast<size_t>(std::abs(maxInt - minInt)))),
      _uniformIntDist{minInt, maxInt},
      _arrNDV(arrNDV),
      _uniformArrSizeDist{minArrLen, maxArrLen},
      _nestedDataDescriptor(std::move(nestedDataDescriptor)) {
    uassert(6660520, "Maximum integer number must be >= the minimum one.", (maxInt >= minInt));
    uassert(6660521, "Maximum string size must be >= the minimum one.", (maxStrLen >= minStrLen));
    uassert(6660522,
            "Array specs must be 0 if there is no array data descriptor.",
            _nestedDataDescriptor || (arrNDV == 0 && minArrLen == 0 && maxArrLen == 0));
    uassert(6660523,
            "Nested arrays requires sensible array lengths",
            !_nestedDataDescriptor || maxArrLen >= minArrLen);
    uassert(6660524, "Recursive descriptors are not allowed.", _nestedDataDescriptor.get() != this);
    uassert(6660525,
            "reuseScalarsRatio is a probability, must be in [0, 1].",
            reuseScalarsRatio >= 0 && reuseScalarsRatio <= 1.0);

    // Compute absolute ranges given relative weights of each value type.
    double sumWeights = 0;
    for (const auto& weightedType : dataTypeDistribution) {
        sumWeights += weightedType.second;
    }
    double sumRelativeWeights = 0;
    auto lastKey = dataTypeDistribution.crbegin()->first;
    for (auto it = dataTypeDistribution.cbegin(); it != dataTypeDistribution.cend(); ++it) {
        const auto weightedType = *it;
        if (weightedType.first != lastKey) {
            sumRelativeWeights += weightedType.second / sumWeights;
            uassert(6660526, "The sum of weights can't be >= 1", sumRelativeWeights < 1);
        } else {
            // Due to rounding errors the last relative weight may not be exactly 1.0. Set it
            // to 1.0.
            sumRelativeWeights = 1.0;
        }
        _dataTypeDistribution.emplace(sumRelativeWeights, weightedType.first);
    }

    // Generate a set of random integers.
    mongo::stdx::unordered_set<int> tmpIntSet;
    tmpIntSet.reserve(_intNDV);
    if (_intNDV == intNDV) {
        for (int i = minInt; i <= maxInt; ++i) {
            tmpIntSet.insert(i);  // This is a dense set of all ints the range.
        }
    } else {
        size_t randCount = 0;
        while (tmpIntSet.size() < _intNDV && randCount < 10 * _intNDV) {
            int randInt = _uniformIntDist(_gen);
            ++randCount;
            tmpIntSet.insert(randInt);
        }
    }
    uassert(
        6660527, "Too few integers generated.", (double)tmpIntSet.size() / (double)_intNDV > 0.99);
    _intSet.reserve(tmpIntSet.size());
    _intSet.insert(_intSet.end(), tmpIntSet.begin(), tmpIntSet.end());
    _uniformIntIdxDist.param(
        std::uniform_int_distribution<size_t>::param_type(0, _intSet.size() - 1));

    // Generate a set of random strings with random sizes so that each string can be chosen
    // multiple times in the test data set.
    _stringSet.reserve(strNDV);
    std::uniform_int_distribution<size_t> uniformStrSizeDistr{minStrLen, maxStrLen};
    for (size_t i = 0; i < strNDV; ++i) {
        size_t len = uniformStrSizeDistr(_gen);
        const auto randStr = genRandomString(len);
        _stringSet.push_back(randStr);
    }
    _uniformStrIdxDist.param(
        std::uniform_int_distribution<size_t>::param_type(0, _stringSet.size() - 1));

    // Generate a set of random arrays that are chosen from when generating array data.
    fillRandomArraySet();
}

std::vector<SBEValue> DatasetDescriptor::genRandomDataset(size_t nElems,
                                                          DatasetDescriptor* parentDesc) {
    std::vector<SBEValue> randValues;
    randValues.reserve(nElems);
    DatasetDescriptor* curDesc = this;

    if (parentDesc) {
        double reuseProb = _uniformRandProbability(_gen);
        if (reuseProb < parentDesc->_reuseScalarsRatio) {
            curDesc = parentDesc;
        }
    }

    for (size_t i = 0; i < nElems; ++i) {
        // Get the data type of the current value to be generated.
        value::TypeTags genTag = this->getRandDataType();
        // Generate a random value of the corresponding type.
        switch (genTag) {
            case value::TypeTags::NumberInt64: {
                size_t idx = curDesc->_uniformIntIdxDist(_gen);
                auto randInt = curDesc->_intSet.at(idx);
                const auto [tag, val] = makeInt64Value(randInt);
                randValues.emplace_back(tag, val);
                break;
            }
            case value::TypeTags::StringBig:
            case value::TypeTags::StringSmall: {
                size_t idx = curDesc->_uniformStrIdxDist(_gen);
                const auto randStr = curDesc->_stringSet.at(idx);
                const auto [tag, val] = value::makeNewString(randStr);
                const auto [copyTag, copyVal] = value::copyValue(tag, val);
                randValues.emplace_back(copyTag, copyVal);
                break;
            }
            case value::TypeTags::Array: {
                if (_nestedDataDescriptor) {
                    const auto randArray = genRandomArray();
                    auto [arrayTag, arrayVal] = value::makeNewArray();
                    value::Array* arr = value::getArrayView(arrayVal);
                    for (const auto& elem : randArray) {
                        const auto [copyTag, copyVal] =
                            value::copyValue(elem.getTag(), elem.getValue());
                        arr->push_back(copyTag, copyVal);
                    }
                    randValues.emplace_back(arrayTag, arrayVal);
                }
                break;
            }
            default:
                uasserted(6660528, "Unsupported data type");
        }
    }

    return randValues;
}

std::string DatasetDescriptor::genRandomString(size_t len) {
    std::string randStr;
    randStr.reserve(len);
    for (size_t i = 0; i < len; ++i) {
        size_t idx = _uniformCharIdxDist(_gen);
        const char ch = _alphabet[idx];
        randStr += ch;
    }

    return randStr;
}

std::vector<SBEValue> DatasetDescriptor::genRandomArray() {
    uassert(6660529,
            "There must be a nested data descriptor for random array generation.",
            _nestedDataDescriptor);
    if (_arrNDV == 0) {
        size_t randArraySize = _uniformArrSizeDist(_gen);
        return _nestedDataDescriptor->genRandomDataset(randArraySize, this);
    } else {
        size_t idx = _uniformArrIdxDist(_gen);
        return _arraySet.at(idx);
    }
}

void DatasetDescriptor::fillRandomArraySet() {
    for (size_t i = 0; i < _arrNDV; ++i) {
        size_t randArraySize = _uniformArrSizeDist(_gen);
        const auto randArray = _nestedDataDescriptor->genRandomDataset(randArraySize, this);
        _arraySet.push_back(randArray);
    }

    if (_arrNDV > 0) {
        _uniformArrIdxDist.param(
            std::uniform_int_distribution<size_t>::param_type(0, _arraySet.size() - 1));
    }
}

/**
    Generate a random string. It is possible (even expected) that the same parameters
    will generate different strings on successive calls
*/
std::string genRandomString(size_t len, std::mt19937_64& gen, size_t seed) {
    std::string randStr;
    randStr.reserve(len);
    const constexpr char* kAlphabet =
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    std::uniform_int_distribution<size_t> uniformDist{0, std::strlen(kAlphabet) - 1};

    for (size_t i = 0; i < len; ++i) {
        size_t idx = uniformDist(gen);
        const char ch = kAlphabet[idx];
        randStr += ch;
    }

    return randStr;
}

/**
    Generate a string. This string will be deterministic in that the same
    parameters will always generate the same string, even on different platforms.
*/
std::string genString(size_t len, size_t seed) {
    std::string str;
    str.reserve(len);

    const constexpr char* kAlphabet =
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    const int kAlphabetLength = strlen(kAlphabet);

    unsigned long long rand = seed;
    for (size_t i = 0; i < len; ++i) {
        // Library implementations of rand vary by compiler, naturally, Since we still
        // want the appearance of randomness, but consistency across compilers, we use a linear
        // congruential generator to choose characters for the string. The parameters chosen
        // are from Numerical Recipes. We use the upper 32 bits when calculating the character
        // index, as the lower 32 are essentially nonrandom -- a weakness of LCGs in general.
        rand = 3935559000370003845ULL * rand + 269134368944950781ULL;

        int idx = (rand >> 32) % kAlphabetLength;
        str += kAlphabet[idx];
    }

    return str;
}

/**
    Generate an array of values with the required ratio of int to string. This array will be
    deterministic in that the same parameters will always generate the same array, even on
    different platforms.
*/
std::vector<SBEValue> genFixedValueArray(size_t nElems,
                                         double intRatio,
                                         double strRatio,
                                         boost::optional<size_t> strLen) {

    std::vector<SBEValue> values;

    const int intNDV = static_cast<int>(nElems) / 4;
    for (size_t i = 0; i < std::round(nElems * intRatio); ++i) {
        const auto [tag, val] = makeInt64Value((i % intNDV) + 1);
        values.emplace_back(tag, val);
    }

    if (strRatio == 0.0) {
        return values;
    }

    // Generate a set of strings so that each string can be chosen multiple times in the test
    // data set.
    const size_t strNDV = nElems / 5;
    std::vector<std::string> stringSet;
    stringSet.reserve(strNDV);
    for (size_t i = 0; i < strNDV; ++i) {
        const auto randStr = genString(strLen.get_value_or(8), i);
        stringSet.push_back(randStr);
    }

    for (size_t i = 0; i < std::round(nElems * strRatio); ++i) {
        size_t idx = i % stringSet.size();
        const auto& randStr = stringSet[idx];
        const auto [tag, val] = value::makeNewString(randStr);
        values.emplace_back(tag, val);
    }

    return values;
}

std::vector<SBEValue> genRandomValueArray(size_t nElems,
                                          double intRatio,
                                          double strRatio,
                                          size_t seed) {
    std::vector<SBEValue> randValues;
    const int intNDV = static_cast<int>(nElems) / 4;
    const size_t strNDV = nElems / 5;
    std::vector<std::string> stringSet;
    stringSet.reserve(strNDV);

    std::mt19937_64 gen{seed};
    std::uniform_int_distribution<int> uniformDist{1, intNDV};

    for (size_t i = 0; i < std::round(nElems * intRatio); ++i) {
        const auto [tag, val] = makeInt64Value(uniformDist(gen));
        randValues.emplace_back(tag, val);
    }

    // Generate a set of strings so that each string can be chosen multiple times in the test
    // data set.
    for (size_t i = 0; i < strNDV; ++i) {
        const auto randStr = genRandomString(8, gen, seed);
        stringSet.push_back(randStr);
    }

    std::uniform_int_distribution<size_t> idxDistr{0, stringSet.size() - 1};
    for (size_t i = 0; i < std::round(nElems * strRatio); ++i) {
        size_t idx = idxDistr(gen);
        const auto& randStr = stringSet[idx];
        const auto [tag, val] = value::makeNewString(randStr);
        randValues.emplace_back(tag, val);
    }

    return randValues;
}

std::vector<SBEValue> nestArrays(const std::vector<SBEValue>& input, size_t emptyArrayCount) {
    std::vector<SBEValue> result;
    auto [arrayTag, arrayVal] = value::makeNewArray();

    for (size_t i = 0; i < input.size(); i++) {
        const auto v = input[i].get();
        const auto [tagCopy, valCopy] = value::copyValue(v.first, v.second);

        if (i % 10 < 5) {
            // 50% of values remain scalar.
            result.emplace_back(tagCopy, valCopy);
        } else {
            // 50% of the values are grouped into arrays of size 10.
            value::Array* arr = value::getArrayView(arrayVal);
            arr->push_back(tagCopy, valCopy);
            if (arr->size() == 10) {
                result.emplace_back(arrayTag, arrayVal);
                std::tie(arrayTag, arrayVal) = value::makeNewArray();
            }
        }
    }

    for (size_t i = 0; i < emptyArrayCount; ++i) {
        auto [emptyArrayTag, emptyArrayVal] = value::makeNewArray();
        result.emplace_back(emptyArrayTag, emptyArrayVal);
    }

    // It's possible that the array still contains something. If it's empty,
    // we can safely release it. If not, append it to the result.
    value::Array* arr = value::getArrayView(arrayVal);
    if (arr->size() > 0) {
        result.emplace_back(arrayTag, arrayVal);
    } else {
        value::releaseValue(arrayTag, arrayVal);
    }

    return result;
}

}  // namespace mongo::stats
