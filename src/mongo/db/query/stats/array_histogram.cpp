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

#include "mongo/db/query/stats/array_histogram.h"
#include "mongo/db/query/stats/value_utils.h"

namespace mongo::stats {
namespace {
TypeCounts mapStatsTypeCountToTypeCounts(std::vector<TypeTag> tc) {
    TypeCounts out;
    for (const auto& t : tc) {
        out.emplace(deserialize(t.getTypeName().toString()), t.getCount());
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

double getTagTypeCount(const TypeCounts& tc, sbe::value::TypeTags tag) {
    auto tagIt = tc.find(tag);
    if (tagIt != tc.end()) {
        return tagIt->second;
    }
    return 0.0;
}

/**
 * By default, aggregates the total number of tag counts in the histogrma together.
 * If 'isHistogrammable' is set to true, then only counts histogrammable types.
 * Otherwise, if 'isHistogrammable' is set to false, then only counts non-histogrammable types.
 **/
double getTotalCount(const TypeCounts& tc, boost::optional<bool> isHistogrammable = boost::none) {
    double total = 0.0;
    for (const auto& [tag, count] : tc) {
        if (isHistogrammable || (*isHistogrammable == canEstimateTypeViaHistogram(tag))) {
            total += count;
        }
    }
    return total;
}

double getNumericTypeCount(const TypeCounts& tc) {
    return getTagTypeCount(tc, sbe::value::TypeTags::NumberInt32) +
        getTagTypeCount(tc, sbe::value::TypeTags::NumberInt64) +
        getTagTypeCount(tc, sbe::value::TypeTags::NumberDouble) +
        getTagTypeCount(tc, sbe::value::TypeTags::NumberDecimal);
}

double getStringTypeCount(const TypeCounts& tc) {
    return getTagTypeCount(tc, sbe::value::TypeTags::StringSmall) +
        getTagTypeCount(tc, sbe::value::TypeTags::StringBig) +
        getTagTypeCount(tc, sbe::value::TypeTags::bsonString);
}

double getTypeBracketTypeCount(const TypeCounts& tc, sbe::value::TypeTags tag) {
    if (sbe::value::isNumber(tag)) {
        return getNumericTypeCount(tc);
    } else if (sbe::value::isString(tag)) {
        return getStringTypeCount(tc);
    } else {
        return getTagTypeCount(tc, tag);
    }
}

void validateHistogramTypeCounts(const TypeCounts& tc, const ScalarHistogram& s) {
    const auto& bounds = s.getBounds();
    const auto& buckets = s.getBuckets();
    size_t i = 0;
    while (i < bounds.size()) {
        // First, we have to aggregate all frequencies for the current type bracket.
        const auto tagL = bounds.getAt(i).first;
        double freq = 0.0;
        for (; i < bounds.size(); i++) {
            const auto tagR = bounds.getAt(i).first;
            // Stop aggregating when the next bound belongs to a different type bracket.
            if (!sameTypeBracket(tagL, tagR)) {
                break;
            } else {
                const auto& bucketR = buckets[i];
                freq += bucketR._equalFreq + bucketR._rangeFreq;
            }
        }

        // We now have the expected frequency for this type bracket. Validate it against 'tc'.
        double tcFreq = getTypeBracketTypeCount(tc, tagL);
        if (tcFreq != freq) {
            uasserted(7131014,
                      str::stream()
                          << "Type count frequency " << tcFreq << " did not match bucket frequency "
                          << freq << " of type bracket for " << tagL);
        }
    }
}

struct ArrayFields {
    const ScalarHistogram& arrayUnique;
    const ScalarHistogram& arrayMin;
    const ScalarHistogram& arrayMax;
    const TypeCounts& typeCounts;
    double emptyArrayCount;
};

void validate(const ScalarHistogram& scalar,
              const TypeCounts& typeCounts,
              boost::optional<ArrayFields> arrayFields,
              double trueCount,
              double falseCount) {
    const double numArrays = getTagTypeCount(typeCounts, sbe::value::TypeTags::Array);
    if (arrayFields) {
        if (numArrays <= 0.0) {
            uasserted(7131010, str::stream() << "Array histogram must have at least one array.");
        }

        // We must have the same number of histogrammable type brackets in all arrays, and each such
        // type bracket must have a min value and a max value. Therefore, both the min and max
        // array histograms should have the same cardinality. Note that this is not a true count of
        // arrays since we could have multiple type-brackets per array.
        const double minArrCard = arrayFields->arrayMin.getCardinality();
        const double maxArrCard = arrayFields->arrayMax.getCardinality();
        if (minArrCard != maxArrCard) {
            uasserted(7131011,
                      str::stream() << "Min array cardinality was " << minArrCard
                                    << " while max array cardinality was " << maxArrCard);
        }

        // Ensure we have at least as many histogrammable types counted in our type counters as
        // histogrammable type-brackets across all arrays (this is counted by 'minArrCard').
        double arrayTypeCount = getTotalCount(arrayFields->typeCounts, true /* histogrammable*/);
        if (minArrCard > arrayTypeCount) {
            uasserted(7131012,
                      str::stream() << "The array type counters count " << arrayTypeCount
                                    << " array values, but the minimum number of arrays we must "
                                       "have according to the min/max histograms is "
                                    << minArrCard);
        }

        // There must be at least as many arrays as there are empty arrays.
        if (numArrays < arrayFields->emptyArrayCount) {
            uasserted(7131013,
                      str::stream()
                          << "The Array type counter counts " << numArrays
                          << " arrays, but the minimum number of arrays we must have according to "
                             "the empty array counter is "
                          << arrayFields->emptyArrayCount);
        }

        // TODO SERVER-71057: validate array histograms once type counters only count each type in
        // an array at most once per array. validateHistogramTypeCounts(getArrayTypeCounts(),
        // getArrayUnique());
    } else if (numArrays > 0) {
        uasserted(7131000, "A scalar ArrayHistogram should not have any arrays in its counters.");
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
    validateHistogramTypeCounts(typeCounts, scalar);
}

}  // namespace

ArrayHistogram::ArrayHistogram()
    : ArrayHistogram(ScalarHistogram::make(),
                     {} /* Type counts. */,
                     0.0 /* True count. */,
                     0.0 /* False count. */) {}

ArrayHistogram::ArrayHistogram(ScalarHistogram scalar,
                               TypeCounts typeCounts,
                               ScalarHistogram arrayUnique,
                               ScalarHistogram arrayMin,
                               ScalarHistogram arrayMax,
                               TypeCounts arrayTypeCounts,
                               double emptyArrayCount,
                               double trueCount,
                               double falseCount)
    : _scalar(std::move(scalar)),
      _typeCounts(std::move(typeCounts)),
      _emptyArrayCount(emptyArrayCount),
      _trueCount(trueCount),
      _falseCount(falseCount),
      _arrayUnique(std::move(arrayUnique)),
      _arrayMin(std::move(arrayMin)),
      _arrayMax(std::move(arrayMax)),
      _arrayTypeCounts(std::move(arrayTypeCounts)) {}

ArrayHistogram::ArrayHistogram(ScalarHistogram scalar,
                               TypeCounts typeCounts,
                               double trueCount,
                               double falseCount)
    : _scalar(std::move(scalar)),
      _typeCounts(std::move(typeCounts)),
      _emptyArrayCount(0.0),
      _trueCount(trueCount),
      _falseCount(falseCount),
      _arrayUnique(boost::none),
      _arrayMin(boost::none),
      _arrayMax(boost::none),
      _arrayTypeCounts(boost::none) {}

std::shared_ptr<const ArrayHistogram> ArrayHistogram::make() {
    // No need to validate an empty histogram.
    return std::shared_ptr<const ArrayHistogram>(new ArrayHistogram());
}

std::shared_ptr<const ArrayHistogram> ArrayHistogram::make(ScalarHistogram scalar,
                                                           TypeCounts typeCounts,
                                                           double trueCount,
                                                           double falseCount,
                                                           bool doValidation) {
    if (doValidation) {
        validate(scalar, typeCounts, boost::none, trueCount, falseCount);
    }
    return std::shared_ptr<const ArrayHistogram>(
        new ArrayHistogram(std::move(scalar), std::move(typeCounts), trueCount, falseCount));
}

std::shared_ptr<const ArrayHistogram> ArrayHistogram::make(ScalarHistogram scalar,
                                                           TypeCounts typeCounts,
                                                           ScalarHistogram arrayUnique,
                                                           ScalarHistogram arrayMin,
                                                           ScalarHistogram arrayMax,
                                                           TypeCounts arrayTypeCounts,
                                                           double emptyArrayCount,
                                                           double trueCount,
                                                           double falseCount,
                                                           bool doValidation) {
    if (doValidation) {
        validate(scalar,
                 typeCounts,
                 ArrayFields{arrayUnique, arrayMin, arrayMax, arrayTypeCounts, emptyArrayCount},
                 trueCount,
                 falseCount);
    }
    return std::shared_ptr<const ArrayHistogram>(new ArrayHistogram(std::move(scalar),
                                                                    std::move(typeCounts),
                                                                    std::move(arrayUnique),
                                                                    std::move(arrayMin),
                                                                    std::move(arrayMax),
                                                                    std::move(arrayTypeCounts),
                                                                    emptyArrayCount,
                                                                    trueCount,
                                                                    falseCount));
}

std::shared_ptr<const ArrayHistogram> ArrayHistogram::make(Statistics stats) {
    // Note that we don't run validation when loading a histogram from the Statistics collection
    // because we already validated this histogram before inserting it.
    const auto scalar = ScalarHistogram::make(stats.getScalarHistogram());
    const auto typeCounts = mapStatsTypeCountToTypeCounts(stats.getTypeCount());
    double trueCount = stats.getTrueCount();
    double falseCount = stats.getFalseCount();

    // If we have ArrayStatistics, we will need to initialize the array-only fields.
    if (auto maybeArrayStats = stats.getArrayStatistics(); maybeArrayStats) {
        return std::shared_ptr<const ArrayHistogram>(
            new ArrayHistogram(std::move(scalar),
                               std::move(typeCounts),
                               ScalarHistogram::make(maybeArrayStats->getUniqueHistogram()),
                               ScalarHistogram::make(maybeArrayStats->getMinHistogram()),
                               ScalarHistogram::make(maybeArrayStats->getMaxHistogram()),
                               mapStatsTypeCountToTypeCounts(maybeArrayStats->getTypeCount()),
                               stats.getEmptyArrayCount(),
                               trueCount,
                               falseCount));
    }

    // If we don't have ArrayStatistics available, we should construct a histogram with only scalar
    // fields.
    return std::shared_ptr<const ArrayHistogram>(
        new ArrayHistogram(std::move(scalar), std::move(typeCounts), trueCount, falseCount));
}

bool ArrayHistogram::isArray() const {
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

std::string ArrayHistogram::toString() const {
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

const ScalarHistogram& ArrayHistogram::getScalar() const {
    return _scalar;
}

const ScalarHistogram& ArrayHistogram::getArrayUnique() const {
    tassert(7131002, "Only an array ArrayHistogram has a unique histogram.", isArray());
    return *_arrayUnique;
}

const ScalarHistogram& ArrayHistogram::getArrayMin() const {
    tassert(7131003, "Only an array ArrayHistogram has a min histogram.", isArray());
    return *_arrayMin;
}

const ScalarHistogram& ArrayHistogram::getArrayMax() const {
    tassert(7131004, "Only an array ArrayHistogram has a max histogram.", isArray());
    return *_arrayMax;
}

const TypeCounts& ArrayHistogram::getTypeCounts() const {
    return _typeCounts;
}

const TypeCounts& ArrayHistogram::getArrayTypeCounts() const {
    tassert(7131005, "Only an array ArrayHistogram has array type counts.", isArray());
    return *_arrayTypeCounts;
}

double ArrayHistogram::getArrayCount() const {
    if (isArray()) {
        double arrayCount = getTypeCount(sbe::value::TypeTags::Array);
        uassert(
            6979503, "Histogram with array data must have at least one array.", arrayCount > 0.0);
        return arrayCount;
    }
    return 0.0;
}

double ArrayHistogram::getTypeCount(sbe::value::TypeTags tag) const {
    return getTagTypeCount(getTypeCounts(), tag);
}

double ArrayHistogram::getArrayTypeCount(sbe::value::TypeTags tag) const {
    return getTagTypeCount(getArrayTypeCounts(), tag);
}

double ArrayHistogram::getTotalTypeCount() const {
    return getTotalCount(getTypeCounts());
}

double ArrayHistogram::getTotalArrayTypeCount() const {
    return getTotalCount(getArrayTypeCounts());
}

BSONObj ArrayHistogram::serialize() const {
    BSONObjBuilder histogramBuilder;

    // Serialize boolean type counters.
    histogramBuilder.append("trueCount", getTrueCount());
    histogramBuilder.append("falseCount", getFalseCount());

    // Serialize empty array counts.
    histogramBuilder.appendNumber("emptyArrayCount", getEmptyArrayCount());

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
                       const std::shared_ptr<const ArrayHistogram> arrayHistogram) {
    BSONObjBuilder builder;
    builder.appendNumber("documents", documents);
    builder.appendElements(arrayHistogram->serialize());
    builder.doneFast();
    return builder.obj();
}

BSONObj makeStatsPath(StringData path,
                      double documents,
                      const std::shared_ptr<const ArrayHistogram> arrayHistogram) {
    BSONObjBuilder builder;
    builder.append("_id", path);
    builder.append("statistics", makeStatistics(documents, arrayHistogram));
    builder.doneFast();
    return builder.obj();
}

}  // namespace mongo::stats
