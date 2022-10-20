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

#pragma once

#include <random>
#include <vector>

#include "value_utils.h"

namespace mongo::ce {

class SBEValue;

// A simple histogram describing the distribution of values of each data type.
using DataTypeDistribution = std::map<value::TypeTags, double>;

/**
    Describes the distribution of a dataset according to type and weight. Other ctor parameters
    are used to describe the various data types which can be emitted and correspond to the fields
    named similarly
 */
class DatasetDescriptor {
public:
    DatasetDescriptor(const DataTypeDistribution& dataTypeDistribution,
                      size_t intNDV,
                      int minInt,
                      int maxInt,
                      size_t strNDV,
                      size_t minStrLen,
                      size_t maxStrLen,
                      std::shared_ptr<DatasetDescriptor> nestedDataDescriptor = nullptr,
                      double reuseScalarsRatio = 0,
                      size_t arrNDV = 0,
                      size_t minArrLen = 0,
                      size_t maxArrLen = 0);

    // Generate a random dataset of 'nElems' according to the data distribution characteristics in
    // this object.
    std::vector<SBEValue> genRandomDataset(size_t nElems, DatasetDescriptor* parentDesc = nullptr);

private:
    // Select a random value data type.
    value::TypeTags getRandDataType() {
        double key = _uniformRandProbability(_gen);
        return (*_dataTypeDistribution.upper_bound(key)).second;
    }

    // Generate a random string with size 'len'.
    std::string genRandomString(size_t len);

    // Generate a random array with length determined uniformly between minArrLen and maxArrLen
    std::vector<SBEValue> genRandomArray();

    // Generate a set of random arrays that are chosen from when generating array data.
    void fillRandomArraySet();

private:
    using InternalDataTypeDistribution = std::map<double, value::TypeTags>;
    /*
     * General distribution charecteristics.
     */

    // Pseudo-random generator.
    std::mt19937_64 _gen;
    // Random probabilities. Used to:
    // - Select Value data types as random indexes in '_dataTypeDistribution'.
    // - Select the source of values - either existing scalars or new.
    std::uniform_real_distribution<double> _uniformRandProbability{0.0, 1.0};
    // Distribution of different SBE data types. There will be %percent values of each type.
    InternalDataTypeDistribution _dataTypeDistribution;
    double _reuseScalarsRatio;

    /*
     * Integer data parameters.
     */

    // Number of distinct integer values.
    const size_t _intNDV;
    // A set of integers to choose from while generating random integers.
    std::vector<int> _intSet;
    // Generator of random integers with uniform distribution.
    std::uniform_int_distribution<int> _uniformIntDist;
    // Generator of random indexes into the set of integers '_intSet'.
    std::uniform_int_distribution<size_t> _uniformIntIdxDist;

    /*
     * String data parameters.
     */

    // All strings draw characters from this alphabet.
    static const std::string _alphabet;
    // A set of random strings to choose from. In theory there can be duplicates, but this is very
    // unlikely. We don't care much if there are a few duplicates anyway.
    std::vector<std::string> _stringSet;
    // Generator of random indexes into the set of characters '_alphabet'.
    std::uniform_int_distribution<size_t> _uniformCharIdxDist{0, _alphabet.size() - 1};
    // Generator of random indexes into the set of strings '_stringSet'.
    std::uniform_int_distribution<size_t> _uniformStrIdxDist;

    /*
     * Array data parameters.
     */

    // Number of distinct arrays.
    // TODO: currently not used. The idea is to use it in the same way as arrays - pre-generate
    // '_arrNDV' arrays, then select randomly from this initial set.
    size_t _arrNDV;
    // Set of arrays to pick from when generating random data.
    std::vector<std::vector<SBEValue>> _arraySet;
    // Generator of random array sizes.
    std::uniform_int_distribution<size_t> _uniformArrSizeDist;
    // Descriptor of the dataset within each array.
    std::shared_ptr<DatasetDescriptor> _nestedDataDescriptor;
    // Generator of random indexes into the set of arrays '_arraySet'.
    std::uniform_int_distribution<size_t> _uniformArrIdxDist;
};  // namespace mongo::ce

/**
    Generate a pseudorandom string of length n
    * The alphabet is fixed as [0-9][a-z][A-Z]
    * Characters are chosed uniformly from the alphabet
    * Randomness is implemented such that it is independent of the platform,
        i.e. given the same length and seed on any platform, we will produce the
        same string.
*/
std::string genString(size_t len, size_t seed);

/**
    Generate a set of elements consisting of strings and ints in the
    requested ratio. The generated array will contain the same values given the same
    inputs on all platforms.
 */
std::vector<SBEValue> genFixedValueArray(size_t nElems, double intRatio, double strRatio);

/**
    Generate a random string of length len.
    * The alphabet is fixed as [0-9][a-z][A-Z].
    * Characters are chosed uniformly from the alphabet.
    * Generated strings are likely to differ by platform, so derived values depending on them
      are also likely to change.
 */
std::string genRandomString(size_t len, std::mt19937_64& gen, size_t seed);


/**
    Generate a uniformly random set of elements consisting of string and ints in the
    requested ratio. The resulting array is very likely to differ between platforms, even
    with the same seed. Thus, derived values are also likely to change.

    Prefer genFixedValueArray when comparing derived values against constants.
 */
std::vector<SBEValue> genRandomValueArray(size_t nElems,
                                          double intRatio,
                                          double strRatio,
                                          size_t seed);

/**
    Generate a set up values consisting of half scalars, and half arrays of length 10.

    Values contained in the result will be drawn from the input vector.
 */
std::vector<SBEValue> nestArrays(const std::vector<SBEValue>& input, size_t emptyArrayCount);

}  // namespace mongo::ce
