// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
