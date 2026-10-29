// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


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
