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


#include "mongo/db/update/update_internal_node.h"

namespace mongo {

// static
std::map<std::string, clonable_ptr<UpdateNode>, pathsupport::cmpPathsAndArrayIndexes>
UpdateInternalNode::createUpdateNodeMapByMerging(
    const std::map<std::string, clonable_ptr<UpdateNode>, pathsupport::cmpPathsAndArrayIndexes>&
        leftMap,
    const std::map<std::string, clonable_ptr<UpdateNode>, pathsupport::cmpPathsAndArrayIndexes>&
        rightMap,
    FieldRef* pathTaken,
    bool wrapFieldNameAsArrayFilterIdentifier) {
    std::map<std::string, clonable_ptr<UpdateNode>, pathsupport::cmpPathsAndArrayIndexes> mergedMap;

    // Get the union of the field names we know about among the leftMap and rightMap.
    stdx::unordered_set<std::string> allFields;
    for (const auto& child : leftMap) {
        allFields.insert(child.first);
    }
    for (const auto& child : rightMap) {
        allFields.insert(child.first);
    }

    // Create an entry in mergedMap for all the fields we found.
    for (const std::string& fieldName : allFields) {
        auto leftChildIt = leftMap.find(fieldName);
        auto rightChildIt = rightMap.find(fieldName);
        UpdateNode* leftChildPtr =
            (leftChildIt != leftMap.end()) ? leftChildIt->second.get() : nullptr;
        UpdateNode* rightChildPtr =
            (rightChildIt != rightMap.end()) ? rightChildIt->second.get() : nullptr;
        invariant(leftChildPtr || rightChildPtr);
        mergedMap.insert(
            std::make_pair(fieldName,
                           copyOrMergeAsNecessary(leftChildPtr,
                                                  rightChildPtr,
                                                  pathTaken,
                                                  fieldName,
                                                  wrapFieldNameAsArrayFilterIdentifier)));
    }

    return mergedMap;
}

// static
std::unique_ptr<UpdateNode> UpdateInternalNode::copyOrMergeAsNecessary(
    UpdateNode* leftNode,
    UpdateNode* rightNode,
    FieldRef* pathTaken,
    const std::string& nextField,
    bool wrapFieldNameAsArrayFilterIdentifier) {
    if (!leftNode && !rightNode) {
        return nullptr;
    } else if (!leftNode) {
        return rightNode->clone();
    } else if (!rightNode) {
        return leftNode->clone();
    } else {
        FieldRef::FieldRefTempAppend tempAppend(
            *pathTaken,
            wrapFieldNameAsArrayFilterIdentifier ? toArrayFilterIdentifier(nextField) : nextField);
        return UpdateNode::createUpdateNodeByMerging(*leftNode, *rightNode, pathTaken);
    }
}
}  // namespace mongo
