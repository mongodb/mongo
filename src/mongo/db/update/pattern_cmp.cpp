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

#include "mongo/db/update/pattern_cmp.h"

#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/jsobj.h"

namespace mongo {
namespace pattern_cmp {

Status checkSortClause(const BSONObj& sortObject) {
    if (sortObject.isEmpty()) {
        return Status(ErrorCodes::BadValue,
                      "The sort pattern is empty when it should be a set of fields.");
    }

    for (auto&& patternElement : sortObject) {
        double orderVal = patternElement.isNumber() ? patternElement.Number() : 0;
        if (orderVal != -1 && orderVal != 1) {
            return Status(ErrorCodes::BadValue, "The sort element value must be either 1 or -1");
        }

        FieldRef sortField(patternElement.fieldName());
        if (sortField.numParts() == 0) {
            return Status(ErrorCodes::BadValue, "The sort field cannot be empty");
        }

        for (size_t i = 0; i < sortField.numParts(); ++i) {
            if (sortField.getPart(i).size() == 0) {
                return Status(ErrorCodes::BadValue,
                              str::stream() << "The sort field is a dotted field "
                                               "but has an empty part: "
                                            << sortField.dottedField());
            }
        }
    }

    return Status::OK();
}

}  // namespace pattern_cmp

PatternElementCmp::PatternElementCmp() = default;

PatternElementCmp::PatternElementCmp(const BSONObj& pattern, const CollatorInterface* collator)
    : sortPattern(pattern.copy()), useWholeValue(sortPattern.hasField("")), collator(collator) {}

bool PatternElementCmp::operator()(const mutablebson::Element& lhs,
                                   const mutablebson::Element& rhs) const {
    namespace dps = ::mongo::dotted_path_support;
    if (useWholeValue) {
        const int comparedValue = lhs.compareWithElement(rhs, collator, false);

        const bool reversed = (sortPattern.firstElement().number() < 0);

        return (reversed ? comparedValue > 0 : comparedValue < 0);
    } else {
        BSONObj lhsObj = lhs.getType() == Object ? lhs.getValueObject() : lhs.getValue().wrap("");
        BSONObj rhsObj = rhs.getType() == Object ? rhs.getValueObject() : rhs.getValue().wrap("");

        BSONObj lhsKey = dps::extractElementsBasedOnTemplate(lhsObj, sortPattern, true);
        BSONObj rhsKey = dps::extractElementsBasedOnTemplate(rhsObj, sortPattern, true);

        return lhsKey.woCompare(rhsKey, sortPattern, false, collator) < 0;
    }
}

PatternValueCmp::PatternValueCmp() = default;

PatternValueCmp::PatternValueCmp(const BSONObj& pattern,
                                 const BSONElement& originalElement,
                                 const CollatorInterface* collator)
    : sortPattern(pattern.copy()),
      useWholeValue(sortPattern.hasField("")),
      originalObj(BSONObj().addField(originalElement).copy()),
      collator(collator) {}

bool PatternValueCmp::operator()(const Value& lhs, const Value& rhs) const {
    namespace dps = ::mongo::dotted_path_support;
    if (useWholeValue) {
        const bool descending = (sortPattern.firstElement().number() < 0);
        return (descending ? ValueComparator(collator).getLessThan()(rhs, lhs)
                           : ValueComparator(collator).getLessThan()(lhs, rhs));
    } else {
        BSONObj lhsObj = lhs.isObject() ? lhs.getDocument().toBson() : lhs.wrap("");
        BSONObj rhsObj = rhs.isObject() ? rhs.getDocument().toBson() : rhs.wrap("");

        BSONObj lhsKey = dps::extractElementsBasedOnTemplate(lhsObj, sortPattern, true);
        BSONObj rhsKey = dps::extractElementsBasedOnTemplate(rhsObj, sortPattern, true);

        return lhsKey.woCompare(rhsKey, sortPattern, false, collator) < 0;
    }
}

// Returns the original element passed into the PatternValueCmp constructor.
BSONElement PatternValueCmp::getOriginalElement() const {
    return originalObj.firstElement();
}

}  // namespace mongo
