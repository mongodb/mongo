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


#include "mongo/db/update/unset_node.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/db/update/storage_validation.h"
#include "mongo/util/assert_util.h"

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

Status UnsetNode::init(BSONElement modExpr, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    // Note that we don't need to store modExpr, because $unset does not do anything with its value.
    invariant(modExpr.ok());
    return Status::OK();
}

ModifierNode::ModifyResult UnsetNode::updateExistingElement(mutablebson::Element* element,
                                                            const FieldRef& elementPath) const {
    auto parent = element->parent();

    invariant(parent.ok());
    if (!parent.isType(BSONType::array)) {
        invariant(element->remove());
    } else {
        // Special case: An $unset on an array element sets it to null instead of removing it from
        // the array.
        invariant(element->setValueNull());
    }

    return ModifyResult::kNormalUpdate;
}

void UnsetNode::validateUpdate(mutablebson::ConstElement updatedElement,
                               mutablebson::ConstElement leftSibling,
                               mutablebson::ConstElement rightSibling,
                               std::uint32_t recursionLevel,
                               ModifyResult modifyResult,
                               bool validateForStorage,
                               bool* containsDotsAndDollarsField) const {
    invariant(modifyResult.type == ModifyResult::kNormalUpdate);

    // We only need to check the left and right sibling to see if the removed element was part of a
    // now invalid DBRef.
    const bool doRecursiveCheck = false;
    const uint32_t recursionLevelForCheck = 0;

    if (leftSibling.ok()) {
        storage_validation::scanDocument(leftSibling,
                                         doRecursiveCheck,
                                         recursionLevelForCheck,
                                         false, /* allowTopLevelDollarPrefixedFields */
                                         validateForStorage,
                                         false, /* isEmbeddedInIdField */
                                         containsDotsAndDollarsField);
    }

    if (rightSibling.ok()) {
        storage_validation::scanDocument(rightSibling,
                                         doRecursiveCheck,
                                         recursionLevelForCheck,
                                         false, /* allowTopLevelDollarPrefixedFields */
                                         validateForStorage,
                                         false, /* isEmbeddedInIdField */
                                         containsDotsAndDollarsField);
    }
}

void UnsetNode::logUpdate(LogBuilderInterface* logBuilder,
                          const RuntimeUpdatePath& pathTaken,
                          mutablebson::Element element,
                          ModifyResult modifyResult,
                          boost::optional<int> createdFieldIdx) const {
    invariant(logBuilder);
    invariant(modifyResult.type == ModifyResult::kNormalUpdate);
    invariant(!createdFieldIdx);

    if (pathTaken.types().back() == RuntimeUpdatePath::ComponentType::kArrayIndex) {
        // If $unset is applied to an array index, the value was set to null.
        invariant(element.getType() == BSONType::null);
        uassertStatusOK(logBuilder->logUpdatedField(pathTaken, element));
    } else {
        uassertStatusOK(logBuilder->logDeletedField(pathTaken));
    }
}

}  // namespace mongo
