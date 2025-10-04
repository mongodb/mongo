/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/update/arithmetic_node.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/mutable_bson/algorithm.h"
#include "mongo/db/exec/mutable_bson/document.h"
#include "mongo/util/safe_num.h"
#include "mongo/util/str.h"

#include <cstdint>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

namespace {
const char* getNameForOp(ArithmeticNode::ArithmeticOp op) {
    switch (op) {
        case ArithmeticNode::ArithmeticOp::kAdd:
            return "increment";
        case ArithmeticNode::ArithmeticOp::kMultiply:
            return "multiply";
        default:
            MONGO_UNREACHABLE;
    }
}
}  // namespace

Status ArithmeticNode::init(BSONElement modExpr,
                            const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    invariant(modExpr.ok());

    if (!modExpr.isNumber()) {
        return Status(ErrorCodes::TypeMismatch,
                      str::stream() << "Cannot " << getNameForOp(_op)
                                    << " with non-numeric argument: {" << modExpr << "}");
    }

    _val = modExpr;
    return Status::OK();
}

ModifierNode::ModifyResult ArithmeticNode::updateExistingElement(
    mutablebson::Element* element, const FieldRef& elementPath) const {
    if (!element->isNumeric()) {
        auto idElem = mutablebson::findFirstChildNamed(element->getDocument().root(), "_id");
        uasserted(ErrorCodes::TypeMismatch,
                  str::stream() << "Cannot apply " << operatorName()
                                << " to a value of non-numeric type. {"
                                << (idElem.ok() ? idElem.toString() : "no id")
                                << "} has the field '" << element->getFieldName()
                                << "' of non-numeric type " << typeName(element->getType()));
    }

    SafeNum originalValue = element->getValueSafeNum();
    SafeNum valueToSet = _val;
    switch (_op) {
        case ArithmeticOp::kAdd:
            valueToSet += originalValue;
            break;
        case ArithmeticOp::kMultiply:
            valueToSet *= originalValue;
            break;
    }

    // If the updated value is identical to the original value, treat this as a no-op. Caveat:
    // if the found element is in a deserialized state, we can't do that.
    if (element->getValue().ok() && valueToSet.isIdentical(originalValue)) {
        return ModifyResult::kNoOp;
    } else if (!valueToSet.isValid()) {
        auto idElem = mutablebson::findFirstChildNamed(element->getDocument().root(), "_id");
        uasserted(ErrorCodes::BadValue,
                  str::stream() << "Failed to apply " << operatorName()
                                << " operations to current value (" << originalValue.debugString()
                                << ") for document {" << (idElem.ok() ? idElem.toString() : "no id")
                                << "}");
    } else {
        invariant(element->setValueSafeNum(valueToSet));
        return ModifyResult::kNormalUpdate;
    }
}

void ArithmeticNode::setValueForNewElement(mutablebson::Element* element) const {
    SafeNum valueToSet = _val;
    switch (_op) {
        case ArithmeticOp::kAdd:
            // valueToSet += 0
            break;
        case ArithmeticOp::kMultiply:
            // This results in a createdValue that has the same type as the original "element"
            // but has a 0 value.
            valueToSet *= SafeNum(static_cast<int32_t>(0));
            break;
    }

    // This can fail if 'valueToSet' is not representable as a 64-bit integer.
    uassertStatusOK(element->setValueSafeNum(valueToSet));
}

}  // namespace mongo
