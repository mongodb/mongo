// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
