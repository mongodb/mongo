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
