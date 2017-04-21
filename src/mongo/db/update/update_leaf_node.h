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
#include "mongo/bson/mutable/element.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/update/log_builder.h"
#include "mongo/db/update/update_node.h"
#include "mongo/db/update_index_data.h"

namespace mongo {

/**
 * A leaf node in the prefix tree of update modifier expressions, representing an update to the
 * value at the end of a path. See comment in class definition of UpdateNode for more details.
 */
class UpdateLeafNode : public UpdateNode {
public:
    UpdateLeafNode() : UpdateNode(Type::Leaf) {}

    /**
     * Initializes the leaf node with the value in 'modExpr'. 'modExpr' is a single modifier
     * expression in the update expression. For example, if the update expression is root = {$set:
     * {'a.b': 5, c: 6}, $inc: {'a.c': 1}}, then a single modifier expression would be
     * root["$set"]["a.b"]. Returns a non-OK status if the value in 'modExpr' is invalid for the
     * type of leaf node. 'modExpr' must not be empty.
     *
     * Some leaf nodes require a collator during initialization. For example, $pull requires a
     * collator to construct a MatchExpression that will be used for applying updates across
     * multiple documents.
     */
    virtual Status init(BSONElement modExpr, const CollatorInterface* collator) = 0;

    /**
     * Applies the update node to 'element', creating the fields in 'pathToCreate' if required by
     * the leaves (i.e. the leaves are not all $unset). 'pathTaken' is the path through the root
     * document to 'element', ending with the field name of 'element'. 'pathToCreate' is the path
     * taken through the UpdateNode tree beyond where the path existed in the document. For example,
     * if the update is {$set: {'a.b.c': 5}}, and the document is {a: {}}, then at the leaf node,
     * pathTaken="a" and pathToCreate="b.c" If there was a positional ($) element in the update
     * expression, 'matchedField' is the index of the array element that caused the query to match
     * the document. 'fromReplication' is provided because some modifiers may ignore certain errors
     * when the update is from replication. Uses the index information in 'indexData' to determine
     * whether indexes are affected. If a LogBuilder is provided, logs the update. Outputs whether
     * the operation was a no-op. Returns a non-OK status if the update node cannot be applied to
     * the document.
     *
     * TODO SERVER-28761: Move this virtual method into UpdateNode.
     */
    virtual Status apply(mutablebson::Element element,
                         FieldRef* pathToCreate,
                         FieldRef* pathTaken,
                         StringData matchedField,
                         bool fromReplication,
                         const UpdateIndexData* indexData,
                         LogBuilder* logBuilder,
                         bool* indexesAffected,
                         bool* noop) = 0;
};

}  // namespace mongo
