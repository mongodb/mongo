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

#include <algorithm>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

#include "mongo/db/query/ce/rand_utils_new.h"

#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/util/assert_util.h"

namespace mongo::ce {

const std::string StrDistribution::_alphabet =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

void DataTypeDistrNew::generate(std::vector<SBEValue>& randValues, std::mt19937_64& gen) {
    if (_nullsRatio > 0 && _nullSelector(gen) < _nullsRatio) {
        auto [tag, val] = makeNullValue();
        randValues.emplace_back(tag, val);
    } else {
        size_t idx = (*_idxDist)(gen);
        const auto val = _valSet.at(idx);
        auto [copyTag, copyVal] = copyValue(val.getTag(), val.getValue());
        randValues.emplace_back(copyTag, copyVal);
    }
}

void DataTypeDistrNew::generate(value::Array* randValueArray, std::mt19937_64& gen) {
    if (_nullsRatio > 0 && _nullSelector(gen) < _nullsRatio) {
        auto [tag, val] = makeNullValue();
        randValueArray->push_back(tag, val);
    } else {
        size_t idx = (*_idxDist)(gen);
        const auto val = _valSet.at(idx);
        auto [copyTag, copyVal] = copyValue(val.getTag(), val.getValue());
        randValueArray->push_back(copyTag, copyVal);
    }
}

IntDistribution::IntDistribution(MixedDistributionDescriptor distrDescriptor,
                                 double weight,
                                 size_t ndv,
                                 int minInt,
                                 int maxInt,
                                 double nullsRatio)
    : DataTypeDistrNew(distrDescriptor,
                       value::TypeTags::NumberInt64,
                       weight,
                       std::min(ndv, static_cast<size_t>(std::abs(maxInt - minInt))),
                       nullsRatio),
      _minInt(minInt),
      _maxInt(maxInt) {
    uassert(6660507, "Maximum integer number must be >= the minimum one.", (maxInt >= minInt));
}

void IntDistribution::init(DatasetDescriptorNew* parentDesc, std::mt19937_64& gen) {
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
    : DataTypeDistrNew(distrDescriptor, value::TypeTags::StringBig, weight, ndv, nullsRatio),
      _minStrLen(minStrLen),
      _maxStrLen(maxStrLen) {
    uassert(6660509, "Maximum string size must be >= the minimum one.", (maxStrLen >= minStrLen));
}

void StrDistribution::init(DatasetDescriptorNew* parentDesc, std::mt19937_64& gen) {
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

ArrDistribution::ArrDistribution(MixedDistributionDescriptor distrDescriptor,
                                 double weight,
                                 size_t ndv,
                                 size_t minArrLen,
                                 size_t maxArrLen,
                                 std::unique_ptr<DatasetDescriptorNew> arrayDataDescriptor,
                                 double reuseScalarsRatio,
                                 double nullsRatio)
    : DataTypeDistrNew(distrDescriptor, value::TypeTags::Array, weight, ndv, nullsRatio),
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

void ArrDistribution::init(DatasetDescriptorNew* parentDesc, std::mt19937_64& gen) {
    uassert(6660513, "There must always be a parent data descriptor.", parentDesc);

    // Extract the per-type probabilities from the parent descriptor, but set the array probability
    // to 0 to avoid self-recursion.
    std::vector<double> parentProbabilities;
    for (const auto& dtd : parentDesc->_dataTypeDistributions) {
        double prob = (dtd->tag() == value::TypeTags::Array) ? 0 : dtd->weight();
        parentProbabilities.push_back(prob);
    }
    std::discrete_distribution<size_t> parentDataTypeSelector;
    parentDataTypeSelector.param(std::discrete_distribution<size_t>::param_type(
        parentProbabilities.begin(), parentProbabilities.end()));

    // Generate _ndv distinct arrays, and store them in _valSet.
    for (size_t i = 0; i < _ndv; ++i) {
        auto [arrayTag, arrayVal] = value::makeNewArray();
        value::Array* arr = value::getArrayView(arrayVal);
        size_t randArraySize = _uniformArrSizeDist(gen);
        arr->reserve(randArraySize);
        // Generate the data for one random array.
        for (size_t j = 0; j < randArraySize; ++j) {
            DataTypeDistrNew* dtd = nullptr;
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

DatasetDescriptorNew::DatasetDescriptorNew(TypeDistrVector dataTypeDistributions,
                                           std::mt19937_64& gen)
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

DataTypeDistrNew* DatasetDescriptorNew::getRandDataTypeDist() {
    size_t idx = _dataTypeSelector(_gen);
    return _dataTypeDistributions[idx].get();
}

std::vector<SBEValue> DatasetDescriptorNew::genRandomDataset(size_t nElems) {
    std::vector<SBEValue> randValues;
    randValues.reserve(nElems);

    for (size_t i = 0; i < nElems; ++i) {
        DataTypeDistrNew* dtd = getRandDataTypeDist();
        dtd->generate(randValues, _gen);
    }

    return randValues;
}


}  // namespace mongo::ce
