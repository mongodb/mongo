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

#include "mongo/db/query/compiler/stats/ce_histogram.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/query/compiler/stats/value_utils.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <cstddef>
#include <functional>
#include <tuple>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo::stats {
namespace {
TypeCounts mapStatsTypeCountToTypeCounts(std::vector<TypeTag> tc) {
    TypeCounts out;
    for (const auto& t : tc) {
        out.emplace(deserialize(std::string{t.getTypeName()}), t.getCount());
    }
    return out;
}

void serializeTypeCounts(const TypeCounts& typeCounts, BSONObjBuilder& bob) {
    BSONArrayBuilder typeCountBuilder(bob.subarrayStart("typeCount"));
    for (const auto& [sbeType, count] : typeCounts) {
        auto typeCount = BSON("typeName" << stats::serialize(sbeType) << "count" << count);
        typeCountBuilder.append(typeCount);
    }
    typeCountBuilder.doneFast();
}

int64_t getTagTypeCount(const TypeCounts& tc, sbe::value::TypeTags tag) {
    auto tagIt = tc.find(tag);
    if (tagIt != tc.end()) {
        return tagIt->second;
    }
    return 0.0;
}

int64_t getNumericTypeCount(const TypeCounts& tc) {
    return getTagTypeCount(tc, sbe::value::TypeTags::NumberInt32) +
        getTagTypeCount(tc, sbe::value::TypeTags::NumberInt64) +
        getTagTypeCount(tc, sbe::value::TypeTags::NumberDouble) +
        getTagTypeCount(tc, sbe::value::TypeTags::NumberDecimal);
}

int64_t getStringTypeCount(const TypeCounts& tc) {
    return getTagTypeCount(tc, sbe::value::TypeTags::StringSmall) +
        getTagTypeCount(tc, sbe::value::TypeTags::StringBig) +
        getTagTypeCount(tc, sbe::value::TypeTags::bsonString);
}

int64_t getTypeBracketTypeCount(const TypeCounts& tc, sbe::value::TypeTags tag) {
    if (sbe::value::isNumber(tag)) {
        return getNumericTypeCount(tc);
    } else if (sbe::value::isString(tag)) {
        return getStringTypeCount(tc);
    } else {
        return getTagTypeCount(tc, tag);
    }
}

/**
 * Helper class to iterate over all of a histogram's type-brackets and their frequencies in order.
 */
class TypeBracketFrequencyIterator {
public:
    TypeBracketFrequencyIterator(const ScalarHistogram& histogram) : histogram(histogram) {}

    bool hasNext() {
        return _bracket < histogram.getBounds().size();
    }

    /**
     * Iterates over the bounds & buckets one type-bracket at the time, starting from the first
     * bucket/bound. It sums up the frequencies of all the buckets in the current type-bracket, then
     * updates the internal counter so the next call to this function will return the left-most type
     * tag and total frequency of the next type-bracket in the histogram as a pair {tag, frequency}.
     * Once there are no more type-brackets left in the histogram, this will return the tag Nothing
     * with a frequency of 0.0, and 'hasNext()' will return false.
     */
    std::pair<sbe::value::TypeTags, int64_t> getNext() {
        const auto& bounds = histogram.getBounds();
        const auto& buckets = histogram.getBuckets();
        if (hasNext()) {
            // Update tag & frequency for the left-most bucket in the current type-bracket.
            const auto tagL = bounds.getAt(_bracket).first;
            const auto& bucketL = buckets[_bracket];
            double freq = bucketL._equalFreq + bucketL._rangeFreq;

            // Increment the bucket counter to look at the next bucket.
            _bracket++;

            // Aggregate all frequencies for the current type bracket.
            for (; hasNext(); _bracket++) {
                // Get the tag for the next bucket.
                const auto tagR = bounds.getAt(_bracket).first;
                if (!sameTypeBracket(tagL, tagR)) {
                    // Stop aggregating when the next bound belongs to a different type bracket.
                    return {tagL, freq};
                } else {
                    // This is the rightmost bucket in the current type-bracket (so far). Update the
                    // frequency counter and look at the next bucket.
                    const auto& bucketR = buckets[_bracket];
                    freq += bucketR._equalFreq + bucketR._rangeFreq;
                }
            }

            // This was the last type-bracket in the histogram. There are no more buckets left.
            return {tagL, freq};
        }
        return {sbe::value::TypeTags::Nothing, 0.0};
    }

    void reset() {
        _bracket = 0;
    }

    const ScalarHistogram& histogram;

private:
    size_t _bracket = 0;
};

/**
 * Validates the type counts per type bracket compared to those in the scalar histogram according to
 * the comparison function isValid(). It takes a type-bracket count from 'tc' as the left argument
 * and one from 's' as a right argument and returns whether or not the two counts are valid relative
 * to each other.
 */
void validateHistogramTypeCounts(const TypeCounts& tc,
                                 const ScalarHistogram& s,
                                 std::function<bool(int64_t /*tc*/, int64_t /*s*/)> isValid,
                                 int64_t nanCount = 0) {

    // Ensure that all histogrammable type brackets are accounted for in the histogram.
    TypeBracketFrequencyIterator it{s};
    sbe::value::TypeTags tag;
    double freq;
    while (it.hasNext()) {
        std::tie(tag, freq) = it.getNext();
        const double tcFreq = getTypeBracketTypeCount(tc, tag);

        if (sbe::value::isNumber(tag)) {
            // We may see a type-bracket tag that is any numeric type (all numerics fall in the
            // same type bracket)  but because we put nans in a separate counter and not in
            // histogram buckets, we should add them to the histogram frequency.
            freq += nanCount;
        }

        if (!isValid(tcFreq, freq)) {
            uasserted(7105700,
                      str::stream() << "Type count frequency " << tcFreq << " of type bracket for "
                                    << tag << " did not match histogram frequency " << freq);
        }
    }

    // Ensure that all histogrammable type counts are accounted for in the type counters.
    // While typeCount include types for nans but nans are in a separate counts the typeCounts needs
    // to subtract nan counts.
    const double totalTC = getTotalCount(tc, true /* histogrammable*/) - nanCount;
    const double totalCard = s.getCardinality();
    if (!isValid(totalTC, totalCard)) {
        uasserted(7105701,
                  str::stream() << "The type counters count " << totalTC
                                << " values, but the histogram frequency is " << totalCard);
    }
}

/**
 * Validates the relationship between two histograms according to the funciton isValid(). It takes a
 * type-bracket count from 'ls' as the left argument and one from 'rs' as a right argument and
 * returns whether or not the two counts are valid relative to each other.
 */
void validateHistogramFrequencies(const ScalarHistogram& ls,
                                  const ScalarHistogram& rs,
                                  std::function<bool(int64_t /*ls*/, int64_t /*rs*/)> isValid) {
    // Ensure that the total cardinality of the histograms is comparatively correct.
    const double cardL = ls.getCardinality();
    const double cardR = rs.getCardinality();
    if (!isValid(cardL, cardR)) {
        uasserted(7105702,
                  str::stream() << "The histogram cardinalities " << cardL << " and " << cardR
                                << " did not match.");
    }

    // Validate all type brackets in both histograms against each other.
    TypeBracketFrequencyIterator itL{ls};
    TypeBracketFrequencyIterator itR{rs};
    sbe::value::TypeTags tagL, tagR;
    double freqL, freqR;
    while (itL.hasNext() && itR.hasNext()) {
        std::tie(tagL, freqL) = itL.getNext();
        std::tie(tagR, freqR) = itR.getNext();

        if (!sameTypeBracket(tagL, tagR)) {
            // Regardless of whether or not 'ls' is valid relative to 'rs', both must have the same
            // number of type-brackets.
            uasserted(7105703,
                      str::stream() << "Histograms had different type-brackets " << tagL << " and "
                                    << tagR << " at the same bound position.");
        }

        if (!isValid(freqL, freqR)) {
            uasserted(7105704,
                      str::stream()
                          << "Histogram frequencies frequencies " << freqL << " and " << freqR
                          << " of type bracket for " << tagL << " did not match.");
        }
    }

    if (itL.hasNext()) {
        uasserted(7105705, "One histogram had more type-brackets than the other.");
    }
}

struct ArrayFields {
    const ScalarHistogram& arrayUnique;
    const ScalarHistogram& arrayMin;
    const ScalarHistogram& arrayMax;
    const TypeCounts& typeCounts;
    int64_t emptyArrayCount;
};

void validate(const ScalarHistogram& scalar,
              const TypeCounts& typeCounts,
              boost::optional<ArrayFields> arrayFields,
              int64_t sampleSize,
              int64_t trueCount,
              int64_t falseCount,
              int64_t nanCount) {
    const int64_t numArrays = getTagTypeCount(typeCounts, sbe::value::TypeTags::Array);
    if (arrayFields) {
        if (numArrays <= 0.0) {
            uasserted(7131010, str::stream() << "Array histogram must have at least one array.");
        }

        // There must be at least as many arrays as there are empty arrays.
        if (numArrays < arrayFields->emptyArrayCount) {
            uasserted(7131011,
                      str::stream()
                          << "The Array type counter counts " << numArrays
                          << " arrays, but the minimum number of arrays we must have according to "
                             "the empty array counter is "
                          << arrayFields->emptyArrayCount);
        }

        // Validate array histograms based on array type counters. Since there is one entry per type
        // bracket per array in the min/max histograms, ensure that there are at least as many
        // histogrammable entries in the array type counts.
        // Note that min/max histograms may have different type-brackets.
        validateHistogramTypeCounts(arrayFields->typeCounts,
                                    arrayFields->arrayMin,
                                    // Type counts are an upper bound on ArrayMin.
                                    std::greater_equal<int64_t>(),
                                    0.0);

        validateHistogramTypeCounts(arrayFields->typeCounts,
                                    arrayFields->arrayMax,
                                    // Type counts are an upper bound on ArrayMax.
                                    std::greater_equal<int64_t>(),
                                    0.0);

        // Conversely, unique histograms are an upper bound on type counts, since they may count
        // multiple values per type bracket. Furthermore, the min/max histograms are a "lower bound"
        // on the unique histogram.
        validateHistogramTypeCounts(arrayFields->typeCounts,
                                    arrayFields->arrayUnique,
                                    // Type counts are a lower bound on ArrayUnique.
                                    std::less_equal<int64_t>(),
                                    0.0);

        validateHistogramFrequencies(arrayFields->arrayMin,
                                     arrayFields->arrayUnique,
                                     // ArrayMin is a lower bound on ArrayUnique.
                                     std::less_equal<int64_t>());
        validateHistogramFrequencies(arrayFields->arrayMax,
                                     arrayFields->arrayUnique,
                                     // ArrayMax is a lower bound on ArrayUnique.
                                     std::less_equal<int64_t>());

    } else if (numArrays > 0) {
        uasserted(7131000, "A scalar CEHistogram should not have any arrays in its counters.");
    }

    // Validate boolean counters.
    const auto expectedBoolCount = trueCount + falseCount;
    if (const auto boolCount = getTagTypeCount(typeCounts, sbe::value::TypeTags::Boolean);
        boolCount != expectedBoolCount) {
        uasserted(7131001,
                  str::stream() << "Expected type count of booleans to be " << expectedBoolCount
                                << ", was " << boolCount);
    }

    // Validate scalar type counts.
    validateHistogramTypeCounts(typeCounts,
                                scalar,
                                // Type-bracket type counts should equal scalar type-bracket counts.
                                std::equal_to<int64_t>(),
                                nanCount);

    // Validate total count.
    const auto totalCard = getTotalCount(typeCounts);
    if (totalCard != sampleSize) {
        uasserted(7261500,
                  str::stream() << "Expected sum of type counts " << totalCard
                                << " to equal sample size " << sampleSize);
    }

    // Validate NaN count. It is invalid to have more NaNs than numeric types that can contain
    // doubles in the type counters.
    auto nanAbleTypeCount = getTagTypeCount(typeCounts, sbe::value::TypeTags::NumberDouble) +
        getTagTypeCount(typeCounts, sbe::value::TypeTags::NumberDecimal);

    if (nanCount > nanAbleTypeCount) {
        uasserted(7289700,
                  str::stream() << "Expected sum of numeric type counts " << nanAbleTypeCount
                                << " to be no less than"
                                << " NaN counts " << nanCount);
    }
}

}  // namespace

int64_t getTotalCount(const TypeCounts& tc, boost::optional<bool> isHistogrammable) {
    int64_t total = 0.0;
    for (const auto& [tag, count] : tc) {
        if (!isHistogrammable || (*isHistogrammable == canEstimateTypeViaHistogram(tag))) {
            total += count;
        }
    }
    return total;
}

CEHistogram::CEHistogram()
    : CEHistogram(ScalarHistogram::make(), {} /* Type counts. */, 0.0 /* Sample size. */) {}

CEHistogram::CEHistogram(ScalarHistogram scalar,
                         TypeCounts typeCounts,
                         ScalarHistogram arrayUnique,
                         ScalarHistogram arrayMin,
                         ScalarHistogram arrayMax,
                         TypeCounts arrayTypeCounts,
                         int64_t sampleSize,
                         int64_t emptyArrayCount,
                         int64_t trueCount,
                         int64_t falseCount,
                         int64_t nanCount)
    : _scalar(std::move(scalar)),
      _typeCounts(std::move(typeCounts)),
      _emptyArrayCount(emptyArrayCount),
      _trueCount(trueCount),
      _falseCount(falseCount),
      _nanCount(nanCount),
      _sampleSize(sampleSize),
      _arrayUnique(std::move(arrayUnique)),
      _arrayMin(std::move(arrayMin)),
      _arrayMax(std::move(arrayMax)),
      _arrayTypeCounts(std::move(arrayTypeCounts)) {}

CEHistogram::CEHistogram(ScalarHistogram scalar,
                         TypeCounts typeCounts,
                         int64_t sampleSize,
                         int64_t trueCount,
                         int64_t falseCount,
                         int64_t nanCount)
    : _scalar(std::move(scalar)),
      _typeCounts(std::move(typeCounts)),
      _emptyArrayCount(0.0),
      _trueCount(trueCount),
      _falseCount(falseCount),
      _nanCount(nanCount),
      _sampleSize(sampleSize),
      _arrayUnique(boost::none),
      _arrayMin(boost::none),
      _arrayMax(boost::none),
      _arrayTypeCounts(boost::none) {}

std::shared_ptr<const CEHistogram> CEHistogram::make() {
    // No need to validate an empty histogram.
    return std::shared_ptr<const CEHistogram>(new CEHistogram());
}

std::shared_ptr<const CEHistogram> CEHistogram::make(ScalarHistogram scalar,
                                                     TypeCounts typeCounts,
                                                     int64_t sampleSize,
                                                     int64_t trueCount,
                                                     int64_t falseCount,
                                                     int64_t nanCount,
                                                     bool doValidation) {
    if (doValidation) {
        validate(scalar, typeCounts, boost::none, sampleSize, trueCount, falseCount, nanCount);
    }
    return std::shared_ptr<const CEHistogram>(new CEHistogram(
        std::move(scalar), std::move(typeCounts), sampleSize, trueCount, falseCount, nanCount));
}

std::shared_ptr<const CEHistogram> CEHistogram::make(ScalarHistogram scalar,
                                                     TypeCounts typeCounts,
                                                     ScalarHistogram arrayUnique,
                                                     ScalarHistogram arrayMin,
                                                     ScalarHistogram arrayMax,
                                                     TypeCounts arrayTypeCounts,
                                                     int64_t sampleSize,
                                                     int64_t emptyArrayCount,
                                                     int64_t trueCount,
                                                     int64_t falseCount,
                                                     int64_t nanCount,
                                                     bool doValidation) {
    if (doValidation) {
        validate(scalar,
                 typeCounts,
                 ArrayFields{arrayUnique, arrayMin, arrayMax, arrayTypeCounts, emptyArrayCount},
                 sampleSize,
                 trueCount,
                 falseCount,
                 nanCount);
    }
    return std::shared_ptr<const CEHistogram>(new CEHistogram(std::move(scalar),
                                                              std::move(typeCounts),
                                                              std::move(arrayUnique),
                                                              std::move(arrayMin),
                                                              std::move(arrayMax),
                                                              std::move(arrayTypeCounts),
                                                              sampleSize,
                                                              emptyArrayCount,
                                                              trueCount,
                                                              falseCount,
                                                              nanCount));
}

std::shared_ptr<const CEHistogram> CEHistogram::make(Statistics stats) {
    // Note that we don't run validation when loading a histogram from the Statistics collection
    // because we already validated this histogram before inserting it.
    const auto scalar = ScalarHistogram::make(stats.getScalarHistogram());
    auto typeCounts = mapStatsTypeCountToTypeCounts(stats.getTypeCount());
    const int64_t trueCount = stats.getTrueCount();
    const int64_t falseCount = stats.getFalseCount();
    const int64_t nanCount = stats.getNanCount();
    const int64_t sampleSize = stats.getDocuments();

    // If we have ArrayStatistics, we will need to initialize the array-only fields.
    if (auto maybeArrayStats = stats.getArrayStatistics(); maybeArrayStats) {
        return std::shared_ptr<const CEHistogram>(
            new CEHistogram(std::move(scalar),
                            std::move(typeCounts),
                            ScalarHistogram::make(maybeArrayStats->getUniqueHistogram()),
                            ScalarHistogram::make(maybeArrayStats->getMinHistogram()),
                            ScalarHistogram::make(maybeArrayStats->getMaxHistogram()),
                            mapStatsTypeCountToTypeCounts(maybeArrayStats->getTypeCount()),
                            sampleSize,
                            stats.getEmptyArrayCount(),
                            trueCount,
                            falseCount,
                            nanCount));
    }

    // If we don't have ArrayStatistics available, we should construct a histogram with only scalar
    // fields.
    return std::shared_ptr<const CEHistogram>(new CEHistogram(
        std::move(scalar), std::move(typeCounts), sampleSize, trueCount, falseCount, nanCount));
}

bool CEHistogram::isArray() const {
    return _arrayUnique && _arrayMin && _arrayMax && _arrayTypeCounts;
}

std::string typeCountsToString(const TypeCounts& typeCounts) {
    std::ostringstream os;
    os << "{";
    bool first = true;
    for (auto [tag, count] : typeCounts) {
        if (!first)
            os << ", ";
        os << tag << ": " << count;
        first = false;
    }
    os << "}";
    return os.str();
}

std::string CEHistogram::toString() const {
    std::ostringstream os;
    os << "{\n";
    os << " scalar: " << _scalar.toString();
    os << ",\n typeCounts: " << typeCountsToString(_typeCounts);
    if (isArray()) {
        os << ",\n arrayUnique: " << _arrayUnique->toString();
        os << ",\n arrayMin: " << _arrayMin->toString();
        os << ",\n arrayMax: " << _arrayMax->toString();
        os << ",\n arrayTypeCounts: " << typeCountsToString(*_arrayTypeCounts);
    }
    os << "\n}\n";
    return os.str();
}

const ScalarHistogram& CEHistogram::getScalar() const {
    return _scalar;
}

const ScalarHistogram& CEHistogram::getArrayUnique() const {
    tassert(7131002, "Only an array CEHistogram has a unique histogram.", isArray());
    return *_arrayUnique;
}

const ScalarHistogram& CEHistogram::getArrayMin() const {
    tassert(7131003, "Only an array CEHistogram has a min histogram.", isArray());
    return *_arrayMin;
}

const ScalarHistogram& CEHistogram::getArrayMax() const {
    tassert(7131004, "Only an array CEHistogram has a max histogram.", isArray());
    return *_arrayMax;
}

const TypeCounts& CEHistogram::getTypeCounts() const {
    return _typeCounts;
}

const TypeCounts& CEHistogram::getArrayTypeCounts() const {
    tassert(7131005, "Only an array CEHistogram has array type counts.", isArray());
    return *_arrayTypeCounts;
}

int64_t CEHistogram::getArrayCount() const {
    if (isArray()) {
        int64_t arrayCount = getTypeCount(sbe::value::TypeTags::Array);
        uassert(
            6979503, "CEHistogram with array data must have at least one array.", arrayCount > 0);
        return arrayCount;
    }
    return 0;
}

int64_t CEHistogram::getTypeCount(sbe::value::TypeTags tag) const {
    return getTagTypeCount(getTypeCounts(), tag);
}

int64_t CEHistogram::getArrayTypeCount(sbe::value::TypeTags tag) const {
    return getTagTypeCount(getArrayTypeCounts(), tag);
}

int64_t CEHistogram::getTotalTypeCount() const {
    return getTotalCount(getTypeCounts());
}

int64_t CEHistogram::getTotalArrayTypeCount() const {
    return getTotalCount(getArrayTypeCounts());
}

BSONObj CEHistogram::serialize() const {
    BSONObjBuilder histogramBuilder;

    // Serialize boolean type counters.
    histogramBuilder.append("trueCount", getTrueCount());
    histogramBuilder.append("falseCount", getFalseCount());
    // Serialize NaN type counter.
    histogramBuilder.append("nanCount", getNanCount());

    // Serialize empty array counts.
    histogramBuilder.append("emptyArrayCount", getEmptyArrayCount());

    // Serialize type counts.
    serializeTypeCounts(getTypeCounts(), histogramBuilder);

    // Serialize scalar histogram.
    histogramBuilder.append("scalarHistogram", getScalar().serialize());

    if (isArray()) {
        // Serialize array histograms and type counts.
        BSONObjBuilder arrayStatsBuilder(histogramBuilder.subobjStart("arrayStatistics"));
        arrayStatsBuilder.append("minHistogram", getArrayMin().serialize());
        arrayStatsBuilder.append("maxHistogram", getArrayMax().serialize());
        arrayStatsBuilder.append("uniqueHistogram", getArrayUnique().serialize());
        serializeTypeCounts(getArrayTypeCounts(), arrayStatsBuilder);
        arrayStatsBuilder.doneFast();
    }

    histogramBuilder.doneFast();
    return histogramBuilder.obj();
}

BSONObj makeStatistics(double documents,
                       double sampleRate,
                       const std::shared_ptr<const CEHistogram> ceHistogram) {
    BSONObjBuilder builder;
    builder.appendNumber("documents", documents);
    builder.appendNumber("sampleRate", sampleRate);
    builder.appendElements(ceHistogram->serialize());
    builder.doneFast();
    return builder.obj();
}

BSONObj makeStatsPath(StringData path,
                      double documents,
                      double sampleRate,
                      const std::shared_ptr<const CEHistogram> ceHistogram) {
    BSONObjBuilder builder;
    builder.append("_id", path);
    builder.append("statistics", makeStatistics(documents, sampleRate, ceHistogram));
    builder.doneFast();
    return builder.obj();
}

}  // namespace mongo::stats
