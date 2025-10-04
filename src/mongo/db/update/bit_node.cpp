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

#include "mongo/db/update/bit_node.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/mutable_bson/algorithm.h"
#include "mongo/db/exec/mutable_bson/document.h"
#include "mongo/util/str.h"

#include <cstdint>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

Status BitNode::init(BSONElement modExpr, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    invariant(modExpr.ok());

    if (modExpr.type() != BSONType::object) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "The $bit modifier is not compatible with a "
                                    << typeName(modExpr.type())
                                    << ". You must pass in an embedded document: "
                                       "{$bit: {field: {and/or/xor: #}}");
    }

    for (const auto& curOp : modExpr.embeddedObject()) {
        const StringData payloadFieldName = curOp.fieldNameStringData();

        BitwiseOp parsedOp;
        if (payloadFieldName == "and") {
            parsedOp.bitOperator = &SafeNum::bitAnd;
        } else if (payloadFieldName == "or") {
            parsedOp.bitOperator = &SafeNum::bitOr;
        } else if (payloadFieldName == "xor") {
            parsedOp.bitOperator = &SafeNum::bitXor;
        } else {
            return Status(ErrorCodes::BadValue,
                          str::stream()
                              << "The $bit modifier only supports 'and', 'or', and 'xor', not '"
                              << payloadFieldName << "' which is an unknown operator: {" << curOp
                              << "}");
        }

        if ((curOp.type() != BSONType::numberInt) && (curOp.type() != BSONType::numberLong)) {
            return Status(ErrorCodes::BadValue,
                          str::stream()
                              << "The $bit modifier field must be an Integer(32/64 bit); a '"
                              << typeName(curOp.type()) << "' is not supported here: {" << curOp
                              << "}");
        }

        parsedOp.operand = SafeNum(curOp);
        _opList.push_back(parsedOp);
    }

    if (_opList.empty()) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "You must pass in at least one bitwise operation. "
                                    << "The format is: "
                                       "{$bit: {field: {and/or/xor: #}}");
    }

    return Status::OK();
}

ModifierNode::ModifyResult BitNode::updateExistingElement(mutablebson::Element* element,
                                                          const FieldRef& elementPath) const {
    if (!element->isIntegral()) {
        mutablebson::Element idElem =
            mutablebson::findFirstChildNamed(element->getDocument().root(), "_id");
        uasserted(ErrorCodes::BadValue,
                  str::stream() << "Cannot apply $bit to a value of non-integral type."
                                << idElem.toString() << " has the field " << element->getFieldName()
                                << " of non-integer type " << typeName(element->getType()));
    }

    SafeNum value = applyOpList(element->getValueSafeNum());

    if (!value.isIdentical(element->getValueSafeNum())) {
        invariant(element->setValueSafeNum(value));
        return ModifyResult::kNormalUpdate;
    } else {
        return ModifyResult::kNoOp;
    }
}

void BitNode::setValueForNewElement(mutablebson::Element* element) const {
    SafeNum value = applyOpList(SafeNum(static_cast<int32_t>(0)));
    invariant(element->setValueSafeNum(value));
}

SafeNum BitNode::applyOpList(SafeNum value) const {
    for (const auto& op : _opList) {
        value = (value.*(op.bitOperator))(op.operand);

        if (!value.isValid()) {
            uasserted(ErrorCodes::BadValue,
                      str::stream() << "Failed to apply $bit operations to current value: "
                                    << value.debugString());
        }
    }

    return value;
}

}  // namespace mongo
