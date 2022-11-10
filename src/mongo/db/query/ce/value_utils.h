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

namespace mongo::ce {

using namespace sbe;

/**
    Container object for SBE value/tag pairs. Supplied values are owned by this object
    and are released on destruction
*/
class SBEValue {
public:
    SBEValue(value::TypeTags tag, value::Value val);
    SBEValue(std::pair<value::TypeTags, value::Value> v);
    ~SBEValue();

    SBEValue(const SBEValue& other);
    SBEValue(SBEValue&& other);

    SBEValue& operator=(const SBEValue& other);
    SBEValue& operator=(SBEValue&& other);

    std::pair<value::TypeTags, value::Value> get() const;
    value::TypeTags getTag() const;
    value::Value getValue() const;

private:
    value::TypeTags _tag;
    value::Value _val;
};

/**
    Generate an SBE Value pair that represents the supplied int with
    type Int64
*/
std::pair<value::TypeTags, value::Value> makeInt64Value(int v);

/**
    Generate an SBE Value pair representing a BSON null value
*/
std::pair<value::TypeTags, value::Value> makeNullValue();

/**
    Do the supplied type tags represent the same BSON type?
*/
bool sameTypeClass(value::TypeTags tag1, value::TypeTags tag2);

/**
    Do the supplied type tags represent the same BSON type?
    TODO: This may be the same as sameTypeClass. @timourk?
*/
bool sameTypeBracket(value::TypeTags tag1, value::TypeTags tag2);

/**
    Compare a pair of SBE values.

    The return will be
        <0 if val1 < val2 in BSON order
        0 if val1 == val2 in BSON order
        >0 if val1 > val2 in BSON order
*/
int32_t compareValues(value::TypeTags tag1,
                      value::Value val1,
                      value::TypeTags tag2,
                      value::Value val2);

/**
    Sort a vector of values in place in BSON order
*/
void sortValueVector(std::vector<SBEValue>& sortVector);

/**
    Convert a value of any supported type into a double according to some metric. This
    metric will be consistent with ordering in the type.
*/
double valueToDouble(value::TypeTags tag, value::Value val);

/**
 * Returns true for types that can be estimated via histograms, and false for types that need type
 * counters. Any other type results in a uassert.
 *
 * NOTE: This should be kept in sync with 'valueToDouble' above.
 */
bool canEstimateTypeViaHistogram(value::TypeTags tag);

}  // namespace mongo::ce
