// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/clonable_ptr.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/update/path_support.h"
#include "mongo/db/update/update_node.h"
#include "mongo/util/modules.h"

#include <map>
#include <string>

namespace mongo {

/**
 * An internal node in the prefix tree of update modifier expressions. See comment in class
 * definition of UpdateNode for more details.
 */
class UpdateInternalNode : public UpdateNode {
public:
    UpdateInternalNode(UpdateNode::Type type) : UpdateNode(type) {}

    /**
     * Returns the child with field name 'field' or nullptr if there is no such child.
     */
    virtual UpdateNode* getChild(const std::string& field) const = 0;

    /**
     * Adds a child with field name 'field'. The node must not already have a child with field
     * name 'field'.
     */
    virtual void setChild(std::string field, std::unique_ptr<UpdateNode> child) = 0;

protected:
    /**
     * Helper for subclass implementations needing to produce the syntax for applied array filters.
     */
    static std::string toArrayFilterIdentifier(const std::string& fieldName) {
        return "$[" + fieldName + "]";
    }

    /**
     * Helper for subclass implementations of createUpdateNodeByMerging. Any UpdateNode value whose
     * key is only in 'leftMap' or only in 'rightMap' is cloned and added to the output map. If the
     * key is in both maps, the two UpdateNodes are merged and added to the output map. If
     * wrapFieldNameAsArrayFilterIdentifier is true, field names are wrapped as $[<field name>] for
     * error reporting.
     */
    static std::map<std::string, clonable_ptr<UpdateNode>, pathsupport::cmpPathsAndArrayIndexes>
    createUpdateNodeMapByMerging(
        const std::map<std::string, clonable_ptr<UpdateNode>, pathsupport::cmpPathsAndArrayIndexes>&
            leftMap,
        const std::map<std::string, clonable_ptr<UpdateNode>, pathsupport::cmpPathsAndArrayIndexes>&
            rightMap,
        FieldRef* pathTaken,
        bool wrapFieldNameAsArrayFilterIdentifier = false);

    /**
     * Helper for subclass implementations of createUpdateNodeByMerging. If one of 'leftNode' or
     * 'rightNode' is non-null, we clone it. If 'leftNode' and 'rightNode' are both non-null, we
     * merge them recursively. If 'leftNode' and 'rightNode' are both null, we return nullptr. If
     * wrapFieldNameAsArrayFilterIdentifier is true, field names are wrapped as $[<field name>] for
     * error reporting.
     */
    static std::unique_ptr<UpdateNode> copyOrMergeAsNecessary(
        UpdateNode* leftNode,
        UpdateNode* rightNode,
        FieldRef* pathTaken,
        const std::string& nextField,
        bool wrapFieldNameAsArrayFilterIdentifier = false);
};

}  // namespace mongo
