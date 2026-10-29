// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/update/update_node.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/update/update_array_node.h"
#include "mongo/db/update/update_object_node.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo {

// static
std::unique_ptr<UpdateNode> UpdateNode::createUpdateNodeByMerging(const UpdateNode& leftNode,
                                                                  const UpdateNode& rightNode,
                                                                  FieldRef* pathTaken) {
    if (leftNode.type == UpdateNode::Type::Object && rightNode.type == UpdateNode::Type::Object) {
        return UpdateObjectNode::createUpdateNodeByMerging(
            static_cast<const UpdateObjectNode&>(leftNode),
            static_cast<const UpdateObjectNode&>(rightNode),
            pathTaken);
    } else if (leftNode.type == UpdateNode::Type::Array &&
               rightNode.type == UpdateNode::Type::Array) {
        return UpdateArrayNode::createUpdateNodeByMerging(
            static_cast<const UpdateArrayNode&>(leftNode),
            static_cast<const UpdateArrayNode&>(rightNode),
            pathTaken);
    } else {
        uasserted(
            ErrorCodes::ConflictingUpdateOperators,
            (str::stream() << "Update created a conflict at '" << pathTaken->dottedField() << "'"));
    }
}

}  // namespace mongo
