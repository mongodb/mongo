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

#include "mongo/bson/bsonelement.h"
#include "mongo/db/update/modifier_table.h"
#include "mongo/db/update/update_node.h"

namespace mongo {

/**
 * An internal node in the prefix tree of update modifier expressions, representing updates to an
 * object. See comment in class definition of UpdateNode for more details.
 */
class UpdateObjectNode : public UpdateNode {

public:
    /**
     * Parses 'modExpr' as an update modifier expression and merges with it with 'root'. Returns a
     * non-OK status if 'modExpr' is not a valid update modifier expression, or if merging would
     * cause a conflict. Returns true if the path of 'modExpr' contains a positional $ element, e.g.
     * 'a.$.b'.
     */
    static StatusWith<bool> parseAndMerge(UpdateObjectNode* root,
                                          modifiertable::ModifierType type,
                                          BSONElement modExpr,
                                          const CollatorInterface* collator);

    UpdateObjectNode() : UpdateNode(Type::Object) {}

    void setCollator(const CollatorInterface* collator) final {
        for (auto&& child : _children) {
            child.second->setCollator(collator);
        }
        _positionalChild->setCollator(collator);
    }

    /**
     * Returns the child with field name 'field' or nullptr if there is no such child.
     */
    UpdateNode* getChild(const std::string& field) const;

    /**
     * Adds a child with field name 'field'. The node must not already have a child with field
     * name 'field'.
     */
    void setChild(std::string field, std::unique_ptr<UpdateNode> child);

private:
    std::map<std::string, std::unique_ptr<UpdateNode>> _children;
    std::unique_ptr<UpdateNode> _positionalChild;
};

}  // namespace mongo
