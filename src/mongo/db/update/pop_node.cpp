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

#include "mongo/db/update/pop_node.h"

#include "mongo/db/matcher/expression_parser.h"

namespace mongo {

Status PopNode::init(BSONElement modExpr, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    auto popVal = MatchExpressionParser::parseIntegerElementToLong(modExpr);
    if (!popVal.isOK()) {
        return popVal.getStatus();
    }
    if (popVal.getValue() != 1LL && popVal.getValue() != -1LL) {
        return {ErrorCodes::FailedToParse,
                str::stream() << "$pop expects 1 or -1, found: " << popVal.getValue()};
    }
    _popFromFront = (popVal.getValue() == -1LL);
    return Status::OK();
}

ModifierNode::ModifyResult PopNode::updateExistingElement(
    mutablebson::Element* element, std::shared_ptr<FieldRef> elementPath) const {
    invariant(element->ok());
    uassert(ErrorCodes::TypeMismatch,
            str::stream() << "Path '" << elementPath->dottedField()
                          << "' contains an element of non-array type '"
                          << typeName(element->getType())
                          << "'",
            element->getType() == BSONType::Array);

    if (!element->hasChildren()) {
        // The path exists and contains an array, but the array is empty.
        return ModifyResult::kNoOp;
    }

    auto elementToRemove = _popFromFront ? element->leftChild() : element->rightChild();
    invariantOK(elementToRemove.remove());

    return ModifyResult::kNormalUpdate;
}

void PopNode::validateUpdate(mutablebson::ConstElement updatedElement,
                             mutablebson::ConstElement leftSibling,
                             mutablebson::ConstElement rightSibling,
                             std::uint32_t recursionLevel,
                             ModifyResult modifyResult) const {
    invariant(modifyResult == ModifyResult::kNormalUpdate);

    // Removing elements from an array cannot increase BSON depth or modify a DBRef, so we can
    // override validateUpdate to do nothing.
}

}  // namespace mongo
