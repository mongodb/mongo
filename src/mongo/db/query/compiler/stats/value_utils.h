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

#include "mongo/base/string_data.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/util/time_support.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace mongo::stats {
/**
    Container object for SBE value/tag pairs. Supplied values are owned by this object
    and are released on destruction
*/
class SBEValue {
public:
    SBEValue(sbe::value::TypeTags tag, sbe::value::Value val);
    SBEValue(std::pair<sbe::value::TypeTags, sbe::value::Value> v);
    ~SBEValue();

    SBEValue(const SBEValue& other);
    SBEValue(SBEValue&& other);

    SBEValue& operator=(const SBEValue& other);
    SBEValue& operator=(SBEValue&& other);

    std::pair<sbe::value::TypeTags, sbe::value::Value> get() const;
    sbe::value::TypeTags getTag() const;
    sbe::value::Value getValue() const;

private:
    sbe::value::TypeTags _tag;
    sbe::value::Value _val;
};

/**
 * Generates an SBE Value pair that represents the supplied int with type Boolean.
 */
std::pair<sbe::value::TypeTags, sbe::value::Value> makeBooleanValue(int64_t);

/**
 * Generates an SBE Value pair that represents the supplied int with type Int64.
 */
std::pair<sbe::value::TypeTags, sbe::value::Value> makeInt64Value(int64_t);

/**
 * Generates an SBE Value pair that represents the supplied int with type Int32.
 */
std::pair<sbe::value::TypeTags, sbe::value::Value> makeInt32Value(int32_t);

/**
 * Generates an SBE Value pair that represents the supplied double.
 */
std::pair<sbe::value::TypeTags, sbe::value::Value> makeDoubleValue(double);

/**
 * Generates an SBE Value pair that represents the supplied date.
 */
std::pair<sbe::value::TypeTags, sbe::value::Value> makeDateValue(Date_t);

/**
 * Generates an SBE Value pair that represents the supplied timestamp.
 */
std::pair<sbe::value::TypeTags, sbe::value::Value> makeTimestampValue(Timestamp);

/**
 * Generates an SBE Value pair representing a BSON null value.
 */
std::pair<sbe::value::TypeTags, sbe::value::Value> makeNullValue();

/**
 * Generates an SBE Value pair representing a NaN value.
 */
std::pair<sbe::value::TypeTags, sbe::value::Value> makeNaNValue();

/**
    Do the supplied type tags represent the same BSON type?
*/
bool sameTypeClass(sbe::value::TypeTags tag1, sbe::value::TypeTags tag2);

/**
    Do the supplied type tags represent the same BSON type?
    TODO: This may be the same as sameTypeClass. @timourk?
*/
bool sameTypeBracket(sbe::value::TypeTags tag1, sbe::value::TypeTags tag2);

/**
    Compare a pair of SBE values.

    The return will be
        <0 if val1 < val2 in BSON order
        0 if val1 == val2 in BSON order
        >0 if val1 > val2 in BSON order
*/
int32_t compareValues(sbe::value::TypeTags tag1,
                      sbe::value::Value val1,
                      sbe::value::TypeTags tag2,
                      sbe::value::Value val2);

bool isEmptyArray(sbe::value::TypeTags tag, sbe::value::Value val);

bool isTrueBool(sbe::value::TypeTags tag, sbe::value::Value val);

/**
 * Sort a vector of values in place in BSON order
 */
void sortValueVector(std::vector<SBEValue>& sortVector);

/**
 * Convert a prefix of the input string (up to 8 characters) to a double.
 */
double stringToDouble(StringData sd);

/**
 * Treats the entire ObjectId as a string of 12 unsigned characters and applies the string-to-double
 * formula to all of them. This preserves SBE ordering.
 */
double objectIdToDouble(const sbe::value::ObjectIdType* sd);

/**
 * Convert a value of any supported type into a double according to some metric. This
 * metric will be consistent with ordering in the type.
 */
double valueToDouble(sbe::value::TypeTags tag, sbe::value::Value val);

/**
 * Given a BSONBuilder building a document, add an additional field with a specific name and value.
 */
void addSbeValueToBSONBuilder(const SBEValue& sbeValue,
                              const std::string& fieldName,
                              BSONObjBuilder& builder);

BSONObj sbeValueVectorToBSON(std::vector<SBEValue>& sbeValues,
                             std::vector<std::string>& fieldNames);

/**
 * Convert a SBEValue of any supported type into a BSONObj.
 */
BSONObj sbeValueToBSON(const SBEValue& sbeValue, const std::string& fieldName);

/**
 * Convert two SBEValues of any supported type into a BSONObj representing an Interval.
 */
BSONObj sbeValuesToInterval(const SBEValue& sbeValueLow,
                            const std::string& fieldNameLow,
                            const SBEValue& sbeValueHigh,
                            const std::string& fieldNameHigh);

/**
 * Returns true for types that can be estimated via histograms, and false for any other type.
 *
 * NOTE: This should be kept in sync with 'valueToDouble' above.
 */
bool canEstimateTypeViaHistogram(sbe::value::TypeTags tag);

/**
 * Returns true if the type can be estimated using either type counts or specific counters. Note
 * that this function does not examine values, even though specific counters exist for certain
 * values such as NaN and [] (empty array). These values are considered in
 * canEstimateIntervalViaTypeCounts().
 *
 * Examples:
 * - canEstimateTypeViaTypeCounts(Boolean): true (via boolean dedicated counters)
 * - canEstimateTypeViaTypeCounts(Null): true (via type counts)
 * - canEstimateTypeViaTypeCounts(NumberInt32): false
 * - canEstimateTypeViaTypeCounts(Array): false
 */
bool canEstimateTypeViaTypeCounts(sbe::value::TypeTags tag);

/**
 * Returns true for value/types combinations that can be estimated via typeCounts, and false for
 * value/type combinations that either need to be estimated via Histogram or cannot be estimated.
 * Any other type results in a uassert.
 */
bool canEstimateIntervalViaTypeCounts(sbe::value::TypeTags startTag,
                                      sbe::value::Value startVal,
                                      bool startInclusive,
                                      sbe::value::TypeTags endTag,
                                      sbe::value::Value endVal,
                                      bool endInclusive);

/**
 * Returns true if the type can be estimated using either histograms or type counts.
 *
 * Examples:
 * - canEstimateType(NumberInt32): true (via histograms)
 * - canEstimateType(Boolean): true (via boolean dedicated counters)
 * - canEstimateType(Null): true (via type counts)
 * - canEstimateType(Array): false
 */
inline bool canEstimateType(sbe::value::TypeTags tag) {
    return canEstimateTypeViaHistogram(tag) || canEstimateTypeViaTypeCounts(tag);
}

/**
 * Serialize/Deserialize a TypeTag to a string for TypeCount storage in the stats collection.
 */
std::string serialize(sbe::value::TypeTags tag);
sbe::value::TypeTags deserialize(const std::string& name);

/**
 * Three ways TypeTags comparison (aka spaceship operator) according to the BSON type sort order.
 */
int compareTypeTags(sbe::value::TypeTags a, sbe::value::TypeTags b);

/**
 * Given a type, returns whether or not that type has a variable width. Returns true if the type has
 * variable width, indicating the maxmimum value cannot be represented by the same type
 * (e.g., objects, arrays). Returns false for types with fixed width (e.g., numbers, booleans).
 */
bool isVariableWidthType(sbe::value::TypeTags tag);

/**
 * Returns the minimum bound of the type 'tag'.
 * The result is a pair where the first element is the minimum bound value (stats::SBEValue), and
 * the second element is a boolean indicating if the bound is inclusive.
 *
 * Examples:
 * - getMinBound(NumberInt32): (nan, true)
 * - getMinBound(StringSmall): ("", true)
 * - getMinBound(Object): ({}, true)
 */
std::pair<stats::SBEValue, bool> getMinBound(sbe::value::TypeTags tag);

/**
 * Returns the maximum bound of the type 'tag'.
 * The result is a pair where the first element is the maximum bound value (stats::SBEValue), and
 * the second element is a boolean indicating if the bound is inclusive.
 *
 * Examples:
 * - getMaxBound(NumberInt32): (infinity, true)
 * - getMaxBound(StringSmall): ({}, false)
 * - getMaxBound(Object): ([], false)
 */
std::pair<stats::SBEValue, bool> getMaxBound(sbe::value::TypeTags tag);


/**
 * Constructs a readable string representation of an interval.
 */
std::string printInterval(bool startInclusive,
                          sbe::value::TypeTags startTag,
                          sbe::value::Value startVal,
                          bool endInclusive,
                          sbe::value::TypeTags endTag,
                          sbe::value::Value endVal);

/**
 * Returns true if the interval is of the same type. This takes type-bracketing into consideration.
 * This helps to determine if the interval is estimable solely from a histogram or a type count.
 *
 * For example, given a find query {a: {$gt: "abc"}} which is translated into interval ["abc", {}),
 * the end bound is type-brackted for String such that it is assigned the minimum value of the next
 * immediate type: Object. Therefore, this returns true for ["abc", {}).
 *
 * NOTE: It returns false on the intervals from inequality on MinKey/MaxKey (e.g. [MinKey, MaxKey)),
 * given that the estimation may often need accesses to both histograms and multiple type counts.
 */
bool sameTypeBracketInterval(sbe::value::TypeTags startTag,
                             bool endInclusive,
                             sbe::value::TypeTags endTag,
                             sbe::value::Value endVal);
/**
 * Returns true if the interval is an empty interval.
 */
bool isEmptyInterval(sbe::value::TypeTags startTag,
                     sbe::value::Value startVal,
                     bool startInclusive,
                     sbe::value::TypeTags endTag,
                     sbe::value::Value endVal,
                     bool endInclusive);

/**
 * Returns true if the interval covers a full type. This helps to determine if the interval is
 * estimable by a type count. The design of this method follows the definition of a full interval as
 * inclusive the minimum value of the current type and either:
 * a) inclusive the maximum value of the current type (if representable, e.g., [nan, inf])
 * or
 * b) exclusive the minimume value of the next type (e.g., for Object, [{}, [])).
 */
bool isFullBracketInterval(sbe::value::TypeTags startTag,
                           sbe::value::Value startVal,
                           bool startInclusive,
                           sbe::value::TypeTags endTag,
                           sbe::value::Value endVal,
                           bool endInclusive);

inline bool operator==(const SBEValue& lhs, const SBEValue& rhs) {
    return compareValues(lhs.getTag(), lhs.getValue(), rhs.getTag(), rhs.getValue()) == 0;
}

/**
 * Divides the interval into multiple sub-intervals, each of which is type-bracketed to satisfy
 * sameTypeBracketInterval(). This allows the first and last sub-intervals to be estimated using
 * either histograms or type counts, while the intermediate sub-intervals, which are fully
 * bracketed, can be estimated using type counts.
 *
 * For the returned sub-intervals to be estimable, the interval must meet one of these conditions:
 * a) It is a same-type bracketed interval estimable via histogram.
 * b) It is estimable via type counts.
 * c) Both bounds are of estimable types.
 */
std::vector<std::pair<std::pair<SBEValue, bool>, std::pair<SBEValue, bool>>> bracketizeInterval(
    sbe::value::TypeTags startTag,
    sbe::value::Value startVal,
    bool startInclusive,
    sbe::value::TypeTags endTag,
    sbe::value::Value endVal,
    bool endInclusive);
}  // namespace mongo::stats
