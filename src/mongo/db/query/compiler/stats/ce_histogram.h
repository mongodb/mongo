// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/compiler/stats/scalar_histogram.h"
#include "mongo/db/query/compiler/stats/stats_for_histograms_gen.h"
#include "mongo/util/modules.h"

#include <map>
#include <memory>
#include <string>
#include <string_view>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo::stats {
using TypeCounts = std::map<sbe::value::TypeTags, int64_t>;

/**
 * By default, aggregates the total number of tag counts in the histogram together.
 * If 'isHistogrammable' is set to true, then only counts histogrammable types.
 * Otherwise, if 'isHistogrammable' is set to false, then only counts non-histogrammable types.
 **/
int64_t getTotalCount(const TypeCounts& tc, boost::optional<bool> isHistogrammable = boost::none);

class CEHistogram {
public:
    /**
     * Factory method for constructing an empty scalar histogram.
     */
    static std::shared_ptr<const CEHistogram> make();

    /**
     * Factory method for constructing an CEHistogram using StatsPath IDL as input.
     */
    static std::shared_ptr<const CEHistogram> make(Statistics stats);

    /**
     * Factory method for constructing a scalar histogram (no array fields).
     */
    static std::shared_ptr<const CEHistogram> make(ScalarHistogram scalar,
                                                   TypeCounts typeCounts,
                                                   int64_t sampleSize,
                                                   int64_t trueCount = 0,
                                                   int64_t falseCount = 0,
                                                   int64_t nanCount = 0,
                                                   bool validate = true);

    /**
     * Factory method for constructing an array field histogram. All array fields must be
     * initialized.
     */
    static std::shared_ptr<const CEHistogram> make(ScalarHistogram scalar,
                                                   TypeCounts typeCounts,
                                                   ScalarHistogram arrayUnique,
                                                   ScalarHistogram arrayMin,
                                                   ScalarHistogram arrayMax,
                                                   TypeCounts arrayTypeCounts,
                                                   int64_t sampleSize,
                                                   int64_t emptyArrayCount = 0,
                                                   int64_t trueCount = 0,
                                                   int64_t falseCount = 0,
                                                   int64_t nanCount = 0,
                                                   bool validate = true);

    // CEHistogram is neither copy-constructible nor copy-assignable.
    CEHistogram(const CEHistogram&) = delete;
    CEHistogram& operator=(const CEHistogram&) = delete;

    // However, it is move-constructible and move-assignable.
    CEHistogram(CEHistogram&&) = default;
    CEHistogram& operator=(CEHistogram&&) = default;
    ~CEHistogram() = default;

    std::string toString() const;

    // Serialize to BSON for storage in stats collection.
    BSONObj serialize() const;

    const ScalarHistogram& getScalar() const;
    const ScalarHistogram& getArrayUnique() const;
    const ScalarHistogram& getArrayMin() const;
    const ScalarHistogram& getArrayMax() const;
    const TypeCounts& getTypeCounts() const;
    const TypeCounts& getArrayTypeCounts() const;

    // Returns whether or not this histogram includes array data points.
    bool isArray() const;

    // Get the total number of arrays in the histogram's path including empty arrays.
    int64_t getArrayCount() const;

    // Get the total number of empty arrays ( [] ) in the histogram's path.
    int64_t getEmptyArrayCount() const {
        return _emptyArrayCount;
    }

    // Get the count of true booleans.
    int64_t getTrueCount() const {
        return _trueCount;
    }

    // Get the count of false booleans.
    int64_t getFalseCount() const {
        return _falseCount;
    }

    // Get the count of NaNs.
    int64_t getNanCount() const {
        return _nanCount;
    }

    // Returns the count of a type as known by the respective type counter. If the type is not
    // present in the TypeCounts map, returns 0.
    int64_t getTypeCount(sbe::value::TypeTags tag) const;
    int64_t getArrayTypeCount(sbe::value::TypeTags tag) const;

    // Returns the sum of counts of all types known by the respective type counter.
    int64_t getTotalTypeCount() const;
    int64_t getTotalArrayTypeCount() const;

    // Returns the proportion of the overall collection that was comprised by the sample used to
    // gnerate this histogram.
    int64_t getSampleSize() const {
        return _sampleSize;
    }

private:
    // Constructs an empty scalar histogram.
    CEHistogram();

    // Constructor for scalar field histograms.
    CEHistogram(ScalarHistogram scalar,
                TypeCounts typeCounts,
                int64_t sampleSize,
                int64_t trueCount = 0,
                int64_t falseCount = 0,
                int64_t nanCount = 0);

    // Constructor for array field histograms. We have to initialize all array fields in this case.
    CEHistogram(ScalarHistogram scalar,
                TypeCounts typeCounts,
                ScalarHistogram arrayUnique,
                ScalarHistogram arrayMin,
                ScalarHistogram arrayMax,
                TypeCounts arrayTypeCounts,
                int64_t sampleSize,
                int64_t emptyArrayCount = 0,
                int64_t trueCount = 0,
                int64_t falseCount = 0,
                int64_t nanCount = 0);

    /* Fields for all paths. */

    // Contains values which appeared originally as scalars on the path.
    ScalarHistogram _scalar;
    // The number of values of each type.
    TypeCounts _typeCounts;
    // The number of empty arrays - they are not accounted for in the histograms.
    int64_t _emptyArrayCount;
    // The counts of true & false booleans.
    int64_t _trueCount;
    int64_t _falseCount;
    // The count of NaNs.
    int64_t _nanCount;
    // The exact number of documents in the sample used to build the histogram.
    int64_t _sampleSize;

    /* Fields for array paths (only initialized if arrays are present). */

    // Contains unique scalar values originating from arrays.
    boost::optional<ScalarHistogram> _arrayUnique;
    // Contains minimum values originating from arrays **per class**.
    boost::optional<ScalarHistogram> _arrayMin;
    // Contains maximum values originating from arrays **per class**.
    boost::optional<ScalarHistogram> _arrayMax;
    // The number of values of each type inside all arrays.
    boost::optional<TypeCounts> _arrayTypeCounts;
};

/**
 * Returns an owned BSON Object representing data matching mongo::Statistics IDL.
 */
BSONObj makeStatistics(double documents,
                       double sampleRate,
                       std::shared_ptr<const CEHistogram> ceHistogram);

/**
 * Returns an owned BSON Object representing data matching mongo::StatsPath IDL.
 */
BSONObj makeStatsPath(std::string_view path,
                      double documents,
                      double sampleRate,
                      std::shared_ptr<const CEHistogram> ceHistogram);
}  // namespace mongo::stats
