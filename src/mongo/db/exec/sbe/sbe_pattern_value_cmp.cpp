/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/sbe_pattern_value_cmp.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/dotted_path/dotted_path_support.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/value.h"

#include <cstdint>

namespace mongo::sbe {
namespace {

/*
In the case where 'tag' is TypeTags::bsonObject, this function will return a BSONObj that points to
the buffer that is doesn't own (the buffer that 'val' points to). The callers therefore should be
careful to make sure that the BSONObj returned from this function does not outlive the Value 'val'.
*/
BSONObj convertValueToBSONObj(value::TypeTags tag, value::Value val) {
    if (tag == value::TypeTags::bsonObject) {
        return BSONObj(value::getRawPointerView(val));
    } else {
        BSONObjBuilder b;
        if (tag == value::TypeTags::Object) {
            bson::convertToBsonObj(b, value::getObjectView(val));
        } else {
            bson::appendValueToBsonObj(b, "", tag, val);
        }
        return b.obj();
    }
}
}  // namespace

SbePatternValueCmp::SbePatternValueCmp() = default;

SbePatternValueCmp::SbePatternValueCmp(value::TypeTags specTag,
                                       value::Value specVal,
                                       const CollatorInterface* collator)
    : sortPattern(convertValueToBSONObj(specTag, specVal)),
      useWholeValue(sortPattern.hasField("")),
      collator(collator),
      reversed(sortPattern.firstElement().number() < 0) {}

bool SbePatternValueCmp::operator()(const std::pair<value::TypeTags, value::Value>& lhs,
                                    const std::pair<value::TypeTags, value::Value>& rhs) const {
    auto [lhsTag, lhsVal] = lhs;
    auto [rhsTag, rhsVal] = rhs;
    if (useWholeValue) {
        auto [comparedTag, comparedVal] =
            value::compareValue(lhsTag, lhsVal, rhsTag, rhsVal, collator);
        if (comparedTag == value::TypeTags::NumberInt32) {
            auto val = value::bitcastTo<int32_t>(comparedVal);
            return (reversed ? val > 0 : val < 0);
        } else {
            return false;
        }
    } else {
        BSONObj lhsObj = convertValueToBSONObj(lhsTag, lhsVal);
        BSONObj rhsObj = convertValueToBSONObj(rhsTag, rhsVal);

        BSONObj lhsKey = ::mongo::bson::extractElementsBasedOnTemplate(lhsObj, sortPattern, true);
        BSONObj rhsKey = ::mongo::bson::extractElementsBasedOnTemplate(rhsObj, sortPattern, true);

        return lhsKey.woCompare(rhsKey, sortPattern, BSONObj::ComparisonRulesSet{0}, collator) < 0;
    }
}

}  // namespace mongo::sbe
