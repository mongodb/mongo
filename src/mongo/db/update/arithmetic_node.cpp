/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/update/arithmetic_node.h"

#include "mongo/bson/mutable/algorithm.h"

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

const char* getModifierNameForOp(ArithmeticNode::ArithmeticOp op) {
    switch (op) {
        case ArithmeticNode::ArithmeticOp::kAdd:
            return "$inc";
        case ArithmeticNode::ArithmeticOp::kMultiply:
            return "$mul";
        default:
            MONGO_UNREACHABLE;
    }
}
}  // namespace

Status ArithmeticNode::init(BSONElement modExpr, const CollatorInterface* collator) {
    invariant(modExpr.ok());

    if (!modExpr.isNumber()) {
        return Status(ErrorCodes::TypeMismatch,
                      str::stream() << "Cannot " << getNameForOp(_op)
                                    << " with non-numeric argument: {"
                                    << modExpr
                                    << "}");
    }

    _val = modExpr;
    return Status::OK();
}

void ArithmeticNode::updateExistingElement(mutablebson::Element* element, bool* noop) const {
    if (!element->isNumeric()) {
        mutablebson::Element idElem =
            mutablebson::findFirstChildNamed(element->getDocument().root(), "_id");
        uasserted(ErrorCodes::TypeMismatch,
                  str::stream() << "Cannot apply " << getModifierNameForOp(_op)
                                << " to a value of non-numeric type. {"
                                << idElem.toString()
                                << "} has the field '"
                                << element->getFieldName()
                                << "' of non-numeric type "
                                << typeName(element->getType()));
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
        *noop = true;
    } else {

        // This can fail if 'valueToSet' is not representable as a 64-bit integer.
        uassertStatusOK(element->setValueSafeNum(valueToSet));
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
