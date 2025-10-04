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

#pragma once

#include "mongo/base/clonable_ptr.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/update/path_support.h"
#include "mongo/db/update/update_node.h"

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
