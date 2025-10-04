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

#include "mongo/db/update/array_culling_node.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/update/storage_validation.h"
#include "mongo/util/assert_util.h"

#include <cstddef>

namespace mongo {

ModifierNode::ModifyResult ArrayCullingNode::updateExistingElement(
    mutablebson::Element* element, const FieldRef& elementPath) const {
    invariant(element->ok());
    uassert(ErrorCodes::BadValue,
            "Cannot apply $pull to a non-array value",
            element->getType() == BSONType::array);

    size_t numRemoved = 0;
    auto cursor = element->leftChild();
    while (cursor.ok()) {
        // Make sure to get the next array element now, because if we remove the 'cursor' element,
        // the rightSibling pointer will be invalidated.
        auto nextElement = cursor.rightSibling();
        if (_matcher->match(cursor)) {
            invariant(cursor.remove());
            numRemoved++;
        }
        cursor = nextElement;
    }

    return (numRemoved == 0) ? ModifyResult::kNoOp : ModifyResult::kNormalUpdate;
}

void ArrayCullingNode::validateUpdate(mutablebson::ConstElement updatedElement,
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
                                     false, /* should validate for storage */
                                     false, /* isEmbeddedInIdField */
                                     containsDotsAndDollarsField);
}

}  // namespace mongo
