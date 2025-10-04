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


#include "mongo/db/update/pop_node.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/update/storage_validation.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

Status PopNode::init(BSONElement modExpr, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    auto popVal = modExpr.parseIntegerElementToLong();
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

ModifierNode::ModifyResult PopNode::updateExistingElement(mutablebson::Element* element,
                                                          const FieldRef& elementPath) const {
    invariant(element->ok());
    uassert(ErrorCodes::TypeMismatch,
            str::stream() << "Path '" << elementPath.dottedField()
                          << "' contains an element of non-array type '"
                          << typeName(element->getType()) << "'",
            element->getType() == BSONType::array);

    if (!element->hasChildren()) {
        // The path exists and contains an array, but the array is empty.
        return ModifyResult::kNoOp;
    }

    auto elementToRemove = _popFromFront ? element->leftChild() : element->rightChild();
    invariant(elementToRemove.remove());

    return ModifyResult::kNormalUpdate;
}

void PopNode::validateUpdate(mutablebson::ConstElement updatedElement,
                             mutablebson::ConstElement leftSibling,
                             mutablebson::ConstElement rightSibling,
                             std::uint32_t recursionLevel,
                             ModifyResult modifyResult,
                             const bool validateForStorage,
                             bool* containsDotsAndDollarsField) const {
    invariant(modifyResult.type == ModifyResult::kNormalUpdate);

    // Removing elements from an array cannot increase BSON depth or modify a DBRef, so we can
    // override validateUpdate to not validate storage constraints but we still want to know if
    // there is any field name containing '.'/'$'.
    bool doRecursiveCheck = true;
    storage_validation::scanDocument(updatedElement,
                                     doRecursiveCheck,
                                     recursionLevel,
                                     false, /* allowTopLevelDollarPrefixedFields */
                                     false, /* Should validate for storage */
                                     false, /* isEmbeddedInIdField */
                                     containsDotsAndDollarsField);
}

}  // namespace mongo
