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
#include "mongo/util/assert_util.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <set>
#include <string>
#include <vector>

#include <fmt/format.h>

namespace mongo::stats {
namespace value = sbe::value;
const std::string StrDistribution::_alphabet =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

void DataTypeDistr::generate(std::vector<SBEValue>& randValues, std::mt19937_64& gen) {
    auto rand = _selector(gen);
    if (_nullsRatio > 0 && rand >= 0 && rand < _nullsRatio) {
        auto [tag, val] = makeNullValue();
        randValues.emplace_back(tag, val);
    } else if (_nanRatio > 0 && rand >= _nullsRatio && rand < _nullsRatio + _nanRatio) {
        auto [tag, val] = stats::makeNaNValue();
        randValues.emplace_back(tag, val);
    } else {
        size_t idx = (*_idxDist)(gen);
        const auto val = _valSet.at(idx);
        auto [copyTag, copyVal] = copyValue(val.getTag(), val.getValue());
        randValues.emplace_back(copyTag, copyVal);
    }
}

void DataTypeDistr::generate(value::Array* randValueArray, std::mt19937_64& gen) {
    auto rand = _selector(gen);
    if (_nullsRatio > 0 && rand < _nullsRatio) {
        auto [tag, val] = makeNullValue();
        randValueArray->push_back(tag, val);
    } else {
        size_t idx = (*_idxDist)(gen);
        const auto val = _valSet.at(idx);
        auto [copyTag, copyVal] = copyValue(val.getTag(), val.getValue());
        randValueArray->push_back(copyTag, copyVal);
    }
}

NullDistribution::NullDistribution(MixedDistributionDescriptor distrDescriptor,
                                   double weight,
                                   size_t ndv)
    : DataTypeDistr(distrDescriptor, value::TypeTags::Null, weight, ndv, 1) {}

void NullDistribution::init(DatasetDescriptor* parentDesc, std::mt19937_64& gen) {}

BooleanDistribution::BooleanDistribution(MixedDistributionDescriptor distrDescriptor,
                                         double weight,
                                         size_t ndv,
                                         bool includeFalse,
                                         bool includeTrue,
                                         double nullsRatio)
    : DataTypeDistr(distrDescriptor, value::TypeTags::NumberInt64, weight, ndv, nullsRatio),
      _includeFalse(includeFalse),
      _includeTrue(includeTrue) {
    uassert(9163901, "At least one of the values needs to be true.", (includeFalse || includeTrue));
}

void BooleanDistribution::init(DatasetDescriptor* parentDesc, std::mt19937_64& gen) {
    _valSet.reserve(2);

    if (_includeFalse) {
        const auto [tagFalse, valFalse] = makeBooleanValue(0);
        _valSet.emplace_back(tagFalse, valFalse);
    }

    if (_includeTrue) {
        const auto [tagTrue, valTrue] = makeBooleanValue(1);
        _valSet.emplace_back(tagTrue, valTrue);
    }
    _idxDist = MixedDistribution::make(_mixedDistrDescriptor, 0, _valSet.size() - 1);
}

IntDistribution::IntDistribution(MixedDistributionDescriptor distrDescriptor,
                                 double weight,
                                 size_t ndv,
                                 int minInt,
                                 int maxInt,
                                 double nullsRatio,
                                 double nanRatio)
    : DataTypeDistr(distrDescriptor,
                    value::TypeTags::NumberInt64,
                    weight,
                    std::min(ndv, static_cast<size_t>(std::abs(maxInt - minInt))),
                    nullsRatio,
                    nanRatio),
      _minInt(minInt),
      _maxInt(maxInt) {
    uassert(6660507, "Maximum integer number must be >= the minimum one.", (maxInt >= minInt));
}

void IntDistribution::init(DatasetDescriptor* parentDesc, std::mt19937_64& gen) {
    std::set<int> tmpIntSet;
    std::uniform_int_distribution<int> uniformIntDist{_minInt, _maxInt};

    if (_ndv == static_cast<size_t>(std::abs(_maxInt - _minInt))) {
        // This is a dense set of all ints in the range.
        for (int i = _minInt; i <= _maxInt; ++i) {
            tmpIntSet.insert(i);
        }
    } else {
        size_t randCount = 0;
        while (tmpIntSet.size() < _ndv && randCount < 10 * _ndv) {
            int randInt = uniformIntDist(gen);
            ++randCount;
            tmpIntSet.insert(randInt);
        }
    }
    uassert(6660508, "Too few integers generated.", (double)tmpIntSet.size() / (double)_ndv > 0.99);
    _valSet.reserve(tmpIntSet.size());
    for (const auto randInt : tmpIntSet) {
        const auto [tag, val] = makeInt64Value(randInt);
        _valSet.emplace_back(tag, val);
    }

    _idxDist = MixedDistribution::make(_mixedDistrDescriptor, 0, _valSet.size() - 1);
}

StrDistribution::StrDistribution(MixedDistributionDescriptor distrDescriptor,
                                 double weight,
                                 size_t ndv,
                                 size_t minStrLen,
                                 size_t maxStrLen,
                                 double nullsRatio)
    : DataTypeDistr(distrDescriptor, value::TypeTags::StringBig, weight, ndv, nullsRatio),
      _minStrLen(minStrLen),
      _maxStrLen(maxStrLen) {
    uassert(6660509, "Maximum string size must be >= the minimum one.", (maxStrLen >= minStrLen));
}

void StrDistribution::init(DatasetDescriptor* parentDesc, std::mt19937_64& gen) {
    // Generate a set of random strings with random sizes between _minStrLen and _maxStrLen.
    _valSet.reserve(_ndv);
    std::uniform_int_distribution<size_t> uniformStrSizeDistr{_minStrLen, _maxStrLen};
    for (size_t i = 0; i < _ndv; ++i) {
        size_t len = uniformStrSizeDistr(gen);
        const auto randStr = genRandomString(len, gen);
        const auto [tag, val] = value::makeNewString(randStr);
        _valSet.emplace_back(tag, val);
    }

    _idxDist = MixedDistribution::make(_mixedDistrDescriptor, 0, _valSet.size() - 1);
}

std::string StrDistribution::genRandomString(size_t len, std::mt19937_64& gen) {
    std::string randStr;
    randStr.reserve(len);
    for (size_t i = 0; i < len; ++i) {
        size_t idx = _uniformCharIdxDist(gen);
        const char ch = _alphabet[idx];
        randStr += ch;
    }

    return randStr;
}

DateDistribution::DateDistribution(MixedDistributionDescriptor distrDescriptor,
                                   double weight,
                                   size_t ndv,
                                   Date_t minDate,
                                   Date_t maxDate,
                                   double nullsRatio)
    : DataTypeDistr(distrDescriptor, value::TypeTags::Date, weight, ndv, nullsRatio),
      _minDate(minDate),
      _maxDate(maxDate) {
    uassert(7169301,
            fmt::format(
                "minDate: {} must be before maxDate: {}", _minDate.toString(), _maxDate.toString()),
            _minDate <= _maxDate);
}

void DateDistribution::init(DatasetDescriptor*, std::mt19937_64& gen) {
    const auto min = _minDate.toMillisSinceEpoch();
    const auto max = _maxDate.toMillisSinceEpoch();
    std::set<long long> tmpRandSet;
    std::uniform_int_distribution<long long> uniformIntDist{min, max};
    while (tmpRandSet.size() < _ndv) {
        long long rand = uniformIntDist(gen);
        tmpRandSet.insert(rand);
    }
    _valSet.reserve(tmpRandSet.size());
    for (const auto rand : tmpRandSet) {
        const auto [tag, val] = makeDateValue(Date_t::fromMillisSinceEpoch(rand));
        _valSet.emplace_back(tag, val);
    }
    _idxDist = MixedDistribution::make(_mixedDistrDescriptor, 0, _valSet.size() - 1);
}

DoubleDistribution::DoubleDistribution(MixedDistributionDescriptor distrDescriptor,
                                       double weight,
                                       size_t ndv,
                                       double min,
                                       double max,
                                       double nullsRatio,
                                       double nanRatio)
    : DataTypeDistr(
          distrDescriptor, value::TypeTags::NumberDouble, weight, ndv, nullsRatio, nanRatio),
      _min(min),
      _max(max) {
    uassert(7169302,
            fmt::format("min double: {} must be less than max double: {}", _min, _max),
            _min <= _max);
}

void DoubleDistribution::init(DatasetDescriptor*, std::mt19937_64& gen) {
    std::set<double> tmpRandSet;
    std::uniform_real_distribution<double> uniformDist{_min, _max};
    while (tmpRandSet.size() < _ndv) {
        tmpRandSet.insert(uniformDist(gen));
    }
    _valSet.reserve(tmpRandSet.size());
    for (const auto rand : tmpRandSet) {
        const auto [tag, val] = makeDoubleValue(rand);
        _valSet.emplace_back(tag, val);
    }
    _idxDist = MixedDistribution::make(_mixedDistrDescriptor, 0, _valSet.size() - 1);
}

ObjectIdDistribution::ObjectIdDistribution(MixedDistributionDescriptor distrDescriptor,
                                           double weight,
                                           size_t ndv,
                                           double nullsRatio)
    : DataTypeDistr(distrDescriptor, value::TypeTags::ObjectId, weight, ndv, nullsRatio) {}

/**
 * Helper to construct a 12-byte ObjectId from three 4-byte integers.
 */
sbe::value::ObjectIdType createObjectId(uint32_t a, uint32_t b, uint32_t c) {
    return {
        static_cast<uint8_t>((a >> 24) & 0xff),
        static_cast<uint8_t>((a >> 16) & 0xff),
        static_cast<uint8_t>((a >> 8) & 0xff),
        static_cast<uint8_t>(a & 0xff),
        static_cast<uint8_t>((b >> 24) & 0xff),
        static_cast<uint8_t>((b >> 16) & 0xff),
        static_cast<uint8_t>((b >> 8) & 0xff),
        static_cast<uint8_t>(b & 0xff),
        static_cast<uint8_t>((c >> 24) & 0xff),
        static_cast<uint8_t>((c >> 16) & 0xff),
        static_cast<uint8_t>((c >> 8) & 0xff),
        static_cast<uint8_t>(c & 0xff),
    };
}

void ObjectIdDistribution::init(DatasetDescriptor*, std::mt19937_64& gen) {
    // An ObjectId is an array of 12 one byte integers.
    // To generate N random ObjectIds, we generate 3*N random four byte integers and use every 3 of
    // those integers to create N ObjectIds.
    std::vector<uint32_t> tmpRandSet;
    std::uniform_int_distribution<uint32_t> uniformDist{0, std::numeric_limits<uint32_t>::max()};
    tmpRandSet.reserve(_ndv * 3);
    for (size_t i = 0; i < _ndv * 3; ++i) {
        tmpRandSet.push_back(uniformDist(gen));
    }
    _valSet.reserve(tmpRandSet.size() / 3);
    for (size_t i = 0; i < tmpRandSet.size(); i += 3) {
        const auto [tag, val] = sbe::value::makeCopyObjectId(
            createObjectId(tmpRandSet[i], tmpRandSet[i + 1], tmpRandSet[i + 2]));
        _valSet.emplace_back(tag, val);
    }
    _idxDist = MixedDistribution::make(_mixedDistrDescriptor, 0, _valSet.size() - 1);
}

ArrDistribution::ArrDistribution(MixedDistributionDescriptor distrDescriptor,
                                 double weight,
                                 size_t ndv,
                                 size_t minArrLen,
                                 size_t maxArrLen,
                                 std::unique_ptr<DatasetDescriptor> arrayDataDescriptor,
                                 double reuseScalarsRatio,
                                 double nullsRatio)
    : DataTypeDistr(distrDescriptor, value::TypeTags::Array, weight, ndv, nullsRatio),
      _uniformArrSizeDist{minArrLen, maxArrLen},
      _arrayDataDescriptor(std::move(arrayDataDescriptor)),
      _reuseScalarsRatio(reuseScalarsRatio) {
    uassert(6660510,
            "Array specs must be 0 if there is no array data descriptor.",
            _arrayDataDescriptor || (ndv == 0 && minArrLen == 0 && maxArrLen == 0));
    uassert(6660511,
            "Nested arrays requires sensible array lengths.",
            !_arrayDataDescriptor || maxArrLen >= minArrLen);
    uassert(6660512,
            "reuseScalarsRatio must be in [0, 1].",
            reuseScalarsRatio >= 0 && reuseScalarsRatio <= 1.0);
}

void ArrDistribution::init(DatasetDescriptor* parentDesc, std::mt19937_64& gen) {
    uassert(6660513, "There must always be a parent data descriptor.", parentDesc);

    // Extract the per-type probabilities from the parent descriptor, only if we need them.
    std::vector<double> parentProbabilities;
    std::discrete_distribution<size_t> parentDataTypeSelector;
    if (_reuseScalarsRatio > 0.0) {
        for (const auto& dtd : parentDesc->_dataTypeDistributions) {
            // Set the array probability to 0 to avoid self-recursion.
            double prob = (dtd->tag() == value::TypeTags::Array) ? 0 : dtd->weight();
            parentProbabilities.push_back(prob);
        }
        parentDataTypeSelector.param(std::discrete_distribution<size_t>::param_type(
            parentProbabilities.begin(), parentProbabilities.end()));
    }

    // Generate _ndv distinct arrays, and store them in _valSet.
    for (size_t i = 0; i < _ndv; ++i) {
        auto [arrayTag, arrayVal] = value::makeNewArray();
        value::Array* arr = value::getArrayView(arrayVal);
        size_t randArraySize = _uniformArrSizeDist(gen);
        arr->reserve(randArraySize);
        // Generate the data for one random array.
        for (size_t j = 0; j < randArraySize; ++j) {
            DataTypeDistr* dtd = nullptr;
            size_t idx;
            double reuseParentProb = _uniformRandProbability(gen);
            if (reuseParentProb < _reuseScalarsRatio) {
                // Pick a random data type descriptor from the parent.
                idx = parentDataTypeSelector(gen);
                dtd = parentDesc->_dataTypeDistributions.at(idx).get();
            } else {
                idx = _arrayDataDescriptor->_dataTypeSelector(gen);
                dtd = _arrayDataDescriptor->_dataTypeDistributions.at(idx).get();
            }
            dtd->generate(arr, gen);
        }
        _valSet.emplace_back(arrayTag, arrayVal);
    }

    _idxDist = MixedDistribution::make(_mixedDistrDescriptor, 0, _valSet.size() - 1);
}

DatasetDescriptor::DatasetDescriptor(TypeDistrVector dataTypeDistributions, std::mt19937_64& gen)
    : _dataTypeDistributions(std::move(dataTypeDistributions)), _gen{gen} {

    // The probability of each type to be chosen. Extracted into a vector in order to setup a
    // discrete_distribution.
    std::vector<double> probabilities;
    probabilities.reserve(_dataTypeDistributions.size());
    for (auto& dtd : _dataTypeDistributions) {
        dtd->init(this, gen);
        probabilities.push_back(dtd->weight());
    }
    _dataTypeSelector.param(
        std::discrete_distribution<size_t>::param_type(probabilities.begin(), probabilities.end()));
}

DataTypeDistr* DatasetDescriptor::getRandDataTypeDist() {
    size_t idx = _dataTypeSelector(_gen);
    return _dataTypeDistributions[idx].get();
}

std::vector<SBEValue> DatasetDescriptor::genRandomDataset(size_t nElems) {
    std::vector<SBEValue> randValues;
    randValues.reserve(nElems);

    for (size_t i = 0; i < nElems; ++i) {
        DataTypeDistr* dtd = getRandDataTypeDist();
        dtd->generate(randValues, _gen);
    }

    return randValues;
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
