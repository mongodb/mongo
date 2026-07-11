// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
                                      bool* containsDotsAndDollarsField,
                                      const bool fromOplogApplication) const {
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
                                     containsDotsAndDollarsField,
                                     fromOplogApplication);
}

}  // namespace mongo
