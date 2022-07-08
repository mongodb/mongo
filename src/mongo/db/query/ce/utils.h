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

/**
 * Note: This is a temporary file used for histogram generation until the histogram implementation
 * is finalized.
 */

namespace mongo::ce {

using namespace sbe;

enum class EstimationType;
class Histogram;

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

const std::pair<value::TypeTags, value::Value> makeInt64Value(int v);
const std::pair<value::TypeTags, value::Value> makeNullValue();

bool sameTypeClass(value::TypeTags tag1, value::TypeTags tag2);

int32_t compareValues3w(value::TypeTags tag1,
                        value::Value val1,
                        value::TypeTags tag2,
                        value::Value val2);

void sortValueVector(std::vector<SBEValue>& sortVector);

}  // namespace mongo::ce
