// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/update/pattern_cmp.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/dotted_path/dotted_path_support.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/exec/mutable_bson/const_element.h"
#include "mongo/db/field_ref.h"
#include "mongo/util/str.h"

#include <cstddef>

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
    namespace dps = ::mongo::bson;
    if (useWholeValue) {
        const int comparedValue = lhs.compareWithElement(rhs, collator, false);

        const bool reversed = (sortPattern.firstElement().number() < 0);

        return (reversed ? comparedValue > 0 : comparedValue < 0);
    } else {
        BSONObj lhsObj =
            lhs.getType() == BSONType::object ? lhs.getValueObject() : lhs.getValue().wrap("");
        BSONObj rhsObj =
            rhs.getType() == BSONType::object ? rhs.getValueObject() : rhs.getValue().wrap("");

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

BSONObj PatternValueCmp::extractSortKey(const Value& val) const {
    namespace dps = ::mongo::bson;
    return val.isObject()
        ? dps::extractElementsBasedOnTemplate(val.getDocument().toBson(), sortPattern, true)
        : dps::extractNullForAllFieldsBasedOnTemplate(sortPattern);
}

bool PatternValueCmp::operator()(const Value& lhs, const Value& rhs) const {
    if (useWholeValue) {
        const bool descending = (sortPattern.firstElement().number() < 0);
        return (descending ? ValueComparator(collator).getLessThan()(rhs, lhs)
                           : ValueComparator(collator).getLessThan()(lhs, rhs));
    } else {
        BSONObj lhsKey = extractSortKey(lhs);
        BSONObj rhsKey = extractSortKey(rhs);
        return lhsKey.woCompare(rhsKey, sortPattern, false, collator) < 0;
    }
}

// Returns the original element passed into the PatternValueCmp constructor.
BSONElement PatternValueCmp::getOriginalElement() const {
    return originalObj.firstElement();
}

}  // namespace mongo
