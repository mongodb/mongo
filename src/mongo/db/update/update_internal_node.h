/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#pragma once

#include <map>
#include <string>

#include "mongo/base/clonable_ptr.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/update/update_node.h"

namespace mongo {

/**
 * An internal node in the prefix tree of update modifier expressions. See comment in class
 * definition of UpdateNode for more details.
 */
class UpdateInternalNode : public UpdateNode {
public:
    /**
     * Helper class for appending to a FieldRef for the duration of the current scope and then
     * restoring the FieldRef at the end of the scope.
     */
    class FieldRefTempAppend {
    public:
        FieldRefTempAppend(FieldRef& fieldRef, StringData part) : _fieldRef(fieldRef) {
            _fieldRef.appendPart(part);
        }

        ~FieldRefTempAppend() {
            _fieldRef.removeLastPart();
        }

    private:
        FieldRef& _fieldRef;
    };

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
     * Helper for subclass implementations of createUpdateNodeByMerging. Any UpdateNode value whose
     * key is only in 'leftMap' or only in 'rightMap' is cloned and added to the output map. If the
     * key is in both maps, the two UpdateNodes are merged and added to the output map. If
     * wrapFieldNameAsArrayFilterIdentifier is true, field names are wrapped as $[<field name>] for
     * error reporting.
     */
    static std::map<std::string, clonable_ptr<UpdateNode>> createUpdateNodeMapByMerging(
        const std::map<std::string, clonable_ptr<UpdateNode>>& leftMap,
        const std::map<std::string, clonable_ptr<UpdateNode>>& rightMap,
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
