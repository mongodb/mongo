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

#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/compiler/stats/value_utils.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/time_support.h"

#include <cmath>
#include <cstddef>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <absl/random/zipf_distribution.h>

namespace mongo::stats {

class DatasetDescriptorNew;

/**
 * A base class for wrappers of STL random distributions that produce size_t values within a range.
 * This class enables polymorphic usage of random distributions, for instance to implement a mix of
 * distributions.
 */
class RandomDistribution {
public:
    RandomDistribution() = default;
    RandomDistribution(const RandomDistribution&) = default;
    RandomDistribution(RandomDistribution&&) = default;
    RandomDistribution& operator=(const RandomDistribution&) = default;
    RandomDistribution& operator=(RandomDistribution&&) = default;
    virtual ~RandomDistribution() = default;

    virtual size_t operator()(std::mt19937_64& gen) = 0;
};

/**
    A uniform random distribution of size_t within a range
 */
class UniformDistr : public RandomDistribution {
public:
    UniformDistr(size_t min, size_t max) : _distr{min, max}, _min(min), _max(max) {}

    size_t operator()(std::mt19937_64& gen) override {
        size_t result = _distr(gen);
        uassert(6660540, "Random index out of range", result >= _min && result <= _max);
        return result;
    }

private:
    std::uniform_int_distribution<size_t> _distr;
    size_t _min;
    size_t _max;
};

/**
 * Wrapper of normal distribution that is guaranteed to produces size_t values within a certain
 * range. The STL class normal_distribution takes a median and standard deviation. This class
 * computes a suitable median and standard deviation from the required [min,max] boundaries.
 */
class NormalDistr : public RandomDistribution {
public:
    NormalDistr(size_t min, size_t max)
        : _distr{(double)(min + max) / 2.0, (double)(max - min) / 4.0},
          _backup{min, max},
          _min((double)min),
          _max((double)max) {}

    size_t operator()(std::mt19937_64& gen) override {
        double randNum = _distr(gen);
        size_t trials = 0;
        // If the result is outside the range (an event with low probability), try 10 more times to
        // get a number in the range.
        while (!(randNum >= _min && randNum <= _max) && trials < 10) {
            randNum = _distr(gen);
            if (randNum < _min) {
                randNum = std::ceil(randNum);
            } else if (randNum > _max) {
                randNum = std::floor(randNum);
            } else {
                randNum = std::round(randNum);
            }
            ++trials;
        }
        if (randNum < _min || randNum > _max) {
            // We couldn't generate a number in [min,max] within 10 attempts. Generate a uniform
            // number.
            randNum = _backup(gen);
        }
        size_t result = std::round(randNum);
        uassert(6660541, "Random index out of range", result >= _min && result <= _max);
        return result;
    }

private:
    std::normal_distribution<double> _distr;
    std::uniform_int_distribution<size_t> _backup;
    double _min;
    double _max;
};

/**
 * Wrapper of zipfian distribution that is guaranteed to produces size_t values within a certain
 * range. The absl::zipf_distribution class requires as input the max value expected in the
 * distribution.
 */
class ZipfianDistr : public RandomDistribution {
public:
    ZipfianDistr(size_t min, size_t max)
        : _distr{max}, _backup{min, max}, _min((double)min), _max((double)max) {}

    size_t operator()(std::mt19937_64& gen) override {
        double randNum = _distr(gen);
        size_t trials = 0;
        // If the result is outside the range (an event with low probability), try 10 more times to
        // get a number in the range.
        while (!(randNum >= _min && randNum <= _max) && trials < 10) {
            randNum = _distr(gen);
            if (randNum < _min) {
                randNum = std::ceil(randNum);
            } else if (randNum > _max) {
                randNum = std::floor(randNum);
            } else {
                randNum = std::round(randNum);
            }
            ++trials;
        }
        if (randNum < _min || randNum > _max) {
            // We couldn't generate a number in [min,max] within 10 attempts. Generate a uniform
            // number.
            randNum = _backup(gen);
        }
        size_t result = std::round(randNum);
        uassert(8871801, "Random index out of range", result >= _min && result <= _max);
        return result;
    }

private:
    absl::zipf_distribution<size_t> _distr;
    std::uniform_int_distribution<size_t> _backup;
    double _min;
    double _max;
};

enum class DistrType { kUniform, kNormal, kZipfian };

using MixedDistributionDescriptor = std::vector<std::pair<DistrType, double /*weight*/>>;

/**
 * Generator for mixed distribution, where mixing is on the type of distribution, in the
 * probabilities specified in distrProbabilites
 */
class MixedDistribution {
public:
    MixedDistribution(std::vector<std::unique_ptr<RandomDistribution>> distrMix,
                      std::vector<double>& distrProbabilities)
        : _distrMix(std::move(distrMix)) {
        _distDist.param(std::discrete_distribution<size_t>::param_type(distrProbabilities.begin(),
                                                                       distrProbabilities.end()));
    }

    static std::unique_ptr<MixedDistribution> make(MixedDistributionDescriptor& descriptor,
                                                   size_t min,
                                                   size_t max) {
        std::vector<double> distrProbabilities;
        std::vector<std::unique_ptr<RandomDistribution>> distrMix;

        for (const auto& [distrType, weight] : descriptor) {
            distrProbabilities.push_back(weight);
            switch (distrType) {
                case DistrType::kUniform:
                    distrMix.emplace_back(std::make_unique<UniformDistr>(min, max));
                    break;
                case DistrType::kNormal:
                    distrMix.emplace_back(std::make_unique<NormalDistr>(min, max));
                    break;
                case DistrType::kZipfian:
                    distrMix.emplace_back(std::make_unique<ZipfianDistr>(min, max));
                    break;
                default:
                    MONGO_UNREACHABLE;
            }
        }

        return std::make_unique<MixedDistribution>(std::move(distrMix), distrProbabilities);
    }

    size_t operator()(std::mt19937_64& gen) {
        size_t distIdx = _distDist(gen);
        size_t result = (*_distrMix.at(distIdx))(gen);
        return result;
    }

private:
    // Mix of different distributions. There can be instances of the same type of distribution,
    // because they can still be defined differently.
    std::vector<std::unique_ptr<RandomDistribution>> _distrMix;
    // Distribution of distributions - select the current distribution with a certain probability.
    std::discrete_distribution<size_t> _distDist;
};

/**
 * Descriptor of a typed data distribution
 */
class DataTypeDistrNew {
public:
    DataTypeDistrNew(MixedDistributionDescriptor distrDescriptor,
                     sbe::value::TypeTags tag,
                     double weight,
                     size_t ndv,
                     double nullsRatio = 0.0,
                     double nanRatio = 0.0)
        : _mixedDistrDescriptor(distrDescriptor),
          _tag(tag),
          _weight(weight),
          _ndv(ndv),
          _nullsRatio(nullsRatio),
          _nanRatio(nanRatio) {
        uassert(6660542, "NDV must be > 0.", ndv > 0);
        uassert(6660543, "nullsRatio must be in [0, 1].", nullsRatio >= 0 && nullsRatio <= 1);
        uassert(9497300, "NaN Ratio must be in [0, 1].", nanRatio >= 0 && nanRatio <= 1);
        uassert(9497301,
                "The sum of NaN Ratio and nullsRatio must be in [0, 1].",
                (nullsRatio + nanRatio) >= 0 && (nullsRatio + nanRatio) <= 1);
    }

    virtual ~DataTypeDistrNew() = default;

    /**
     * Generate all unique values that generation chooses from, and store them in '_valSet'.
     * Different data types provide different implementations.
     * @todo: The 'parentDesc' parameter is used only by array generation. Consider a different way
     * of passing it only to that type.
     */
    virtual void init(DatasetDescriptorNew* parentDesc, std::mt19937_64& gen) = 0;

    /**
     * Generate a single random value, and store it in 'randValues' vector.
     */
    void generate(std::vector<SBEValue>& randValues, std::mt19937_64& gen);

    /**
     * Generate a single random value, and store it in 'randValueArray' array.
     */
    void generate(sbe::value::Array* randValueArray, std::mt19937_64& gen);

    /**
     * Custom equality comparison for storage in sets. There can be only datatype in a set.
     */
    bool operator==(const DataTypeDistrNew& d) const {
        return this->_tag == d._tag;
    }

    sbe::value::TypeTags tag() const {
        return _tag;
    }

    double weight() const {
        return _weight;
    }

protected:
    MixedDistributionDescriptor _mixedDistrDescriptor;
    sbe::value::TypeTags _tag;
    // Weight that determines the probability of a value of this type.
    const double _weight;
    const size_t _ndv;
    // A set of (randomly generated) values to choose from when generating random datasets.
    std::vector<SBEValue> _valSet;
    // Generator of random indexes into a set of values.
    // std::uniform_int_distribution<size_t> _idxDist;
    std::unique_ptr<MixedDistribution> _idxDist;
    // Percent of null values in the dataset.
    double _nullsRatio;
    // Percent of NaN values in the dataset.
    double _nanRatio;
    // Random generator to decide between null/NaN/some number vaule.
    std::uniform_real_distribution<double> _selector{0, 1};

    friend class DatasetDescriptorNew;
};

using TypeDistrVector = std::vector<std::unique_ptr<DataTypeDistrNew>>;

/**
 * Null data distribution.
 */
class NullDistribution : public DataTypeDistrNew {
public:
    NullDistribution(MixedDistributionDescriptor distrDescriptor, double weight, size_t ndv);

    /*
     * Generate a set of null values, and store them in _valSet.
     */
    void init(DatasetDescriptorNew* parentDesc, std::mt19937_64& gen) override;
};

/**
 * Boolean data distribution.
 */
class BooleanDistribution : public DataTypeDistrNew {
public:
    BooleanDistribution(MixedDistributionDescriptor distrDescriptor,
                        double weight,
                        size_t ndv,
                        bool includeFalse,
                        bool includeTrue,
                        double nullsRatio = 0);

    /*
     * Generate a set of random booleans, and store them in _valSet.
     */
    void init(DatasetDescriptorNew* parentDesc, std::mt19937_64& gen) override;

protected:
    // _includeFalse and _includeTrue define which of true/false values will appear in the dataset.
    // if _includeFalse is true, then 'false' values will be generated, otherwise not.
    // Similarly for _includeTrue.
    bool _includeFalse;
    bool _includeTrue;
};

/**
 * Integer data distribution.
 */
class IntDistribution : public DataTypeDistrNew {
public:
    IntDistribution(MixedDistributionDescriptor distrDescriptor,
                    double weight,
                    size_t ndv,
                    int minInt,
                    int maxInt,
                    double nullsRatio = 0,
                    double nanRatio = 0);

    /*
     * Generate a set of random integers, and store them in _valSet.
     */
    void init(DatasetDescriptorNew* parentDesc, std::mt19937_64& gen) override;

protected:
    int _minInt;
    int _maxInt;
};

/**
 * String data distribution.
 */
class StrDistribution : public DataTypeDistrNew {
public:
    StrDistribution(MixedDistributionDescriptor distrDescriptor,
                    double weight,
                    size_t ndv,
                    size_t minStrLen,
                    size_t maxStrLen,
                    double nullsRatio = 0);

    /*
     * Generate a set of random strings, and store them in _valSet.
     */
    void init(DatasetDescriptorNew* parentDesc, std::mt19937_64& gen) override;

protected:
    std::string genRandomString(size_t len, std::mt19937_64& gen);

    size_t _minStrLen;
    size_t _maxStrLen;
    // All strings draw characters from this alphabet.
    static const std::string _alphabet;
    // Generator of random indexes into the set of characters '_alphabet'.
    std::uniform_int_distribution<size_t> _uniformCharIdxDist{0, _alphabet.size() - 1};
};

/**
 * Date data distribution.
 */
class DateDistribution : public DataTypeDistrNew {
public:
    DateDistribution(MixedDistributionDescriptor distrDescriptor,
                     double weight,
                     size_t ndv,
                     Date_t minDate,
                     Date_t maxDate,
                     double nullsRatio = 0);

    void init(DatasetDescriptorNew*, std::mt19937_64& gen) override;

protected:
    Date_t _minDate;
    Date_t _maxDate;
};

/**
 * Double data distribution.
 */
class DoubleDistribution : public DataTypeDistrNew {
public:
    DoubleDistribution(MixedDistributionDescriptor distrDescriptor,
                       double weight,
                       size_t ndv,
                       double min,
                       double max,
                       double nullsRatio = 0,
                       double nanRatio = 0);

    void init(DatasetDescriptorNew*, std::mt19937_64& gen) override;

protected:
    double _min;
    double _max;
};

/**
 * ObjectId data distribution.
 */
class ObjectIdDistribution : public DataTypeDistrNew {
public:
    ObjectIdDistribution(MixedDistributionDescriptor distrDescriptor,
                         double weight,
                         size_t ndv,
                         double nullsRatio = 0);

    void init(DatasetDescriptorNew*, std::mt19937_64& gen) override;
};

/**
 * SBE array data distribution.
 */
class ArrDistribution : public DataTypeDistrNew {
public:
    ArrDistribution(MixedDistributionDescriptor distrDescriptor,
                    double weight,
                    size_t ndv,
                    size_t minArrLen,
                    size_t maxArrLen,
                    std::unique_ptr<DatasetDescriptorNew> arrayDataDescriptor,
                    double reuseScalarsRatio = 0,
                    double nullsRatio = 0);

private:
    void init(DatasetDescriptorNew* parentDesc, std::mt19937_64& gen) override;

    // Generator of random array sizes.
    std::uniform_int_distribution<size_t> _uniformArrSizeDist;
    // Descriptor of the dataset within each array.
    std::unique_ptr<DatasetDescriptorNew> _arrayDataDescriptor;
    // Randomly select a parent or a child distribution when generating random
    std::uniform_real_distribution<double> _uniformRandProbability{0.0, 1.0};
    double _reuseScalarsRatio;
};

/**
    Given a list of tyoed data distibutions, this class is used to generate a vector of values
    according to the distribution weights.
*/
class DatasetDescriptorNew {
public:
    DatasetDescriptorNew(TypeDistrVector dataTypeDistributions, std::mt19937_64& gen);

    // Generate a random dataset of 'nElems' according to the data distribution characteristics in
    // this object.
    std::vector<SBEValue> genRandomDataset(size_t nElems);

private:
    // Select a random value data type.
    DataTypeDistrNew* getRandDataTypeDist();

    // Distribution of different SBE data types. There will be %percent values of each type.
    // TODO: is it a better idea to store shared_ptr or raw pointers to enable reuse?
    TypeDistrVector _dataTypeDistributions;
    // Pseudo-random generator.
    std::mt19937_64& _gen;
    // Select a random data type distribution.
    std::discrete_distribution<size_t> _dataTypeSelector;

    friend class ArrDistribution;
};

}  // namespace mongo::stats
