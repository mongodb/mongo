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

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/util/time_support.h"

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

/**
    Sort a vector of values in place in BSON order
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
    Convert a value of any supported type into a double according to some metric. This
    metric will be consistent with ordering in the type.
*/
double valueToDouble(sbe::value::TypeTags tag, sbe::value::Value val);

/**
 * Returns true for types that can be estimated via histograms, and false for types that need type
 * counters. Any other type results in a uassert.
 *
 * NOTE: This should be kept in sync with 'valueToDouble' above.
 */
bool canEstimateTypeViaHistogram(sbe::value::TypeTags tag);

/**
 * Serialize/Deserialize a TypeTag to a string for TypeCount storage in the stats collection.
 */
std::string serialize(sbe::value::TypeTags tag);
sbe::value::TypeTags deserialize(const std::string& name);

}  // namespace mongo::stats
