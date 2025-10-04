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

#include "mongo/db/update/update_leaf_node.h"

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mongo {

/**
 * Represents a placeholder update that generates conflicts with other updates on the same path but
 * has no effect of its own (i.e., its apply() function does nothing).
 *
 * We use the ConflictPlaceholderNode to ensure that the "from" path of a $rename does not conflict
 * with any other updates. A {$rename: {'from': 'to'}} results in a ConflictPlaceholderNode on the
 * "from" path and a RenameNode on the "to" path. The actual rename operation executes with
 * RenameNode::apply().
 */
class ConflictPlaceholderNode : public UpdateLeafNode {
public:
    Status init(BSONElement modExpr, const boost::intrusive_ptr<ExpressionContext>& expCtx) final {
        return Status::OK();
    }

    std::unique_ptr<UpdateNode> clone() const final {
        return std::make_unique<ConflictPlaceholderNode>(*this);
    }

    void setCollator(const CollatorInterface* collator) final {}

    ApplyResult apply(ApplyParams applyParams,
                      UpdateNodeApplyParams updateNodeApplyParams) const final {
        return ApplyResult::noopResult();
    }

    /**
     * These internally-generated nodes do not need to be serialized.
     */
    void produceSerializationMap(
        FieldRef* currentPath,
        std::map<std::string, std::vector<std::pair<std::string, BSONObj>>>*
            operatorOrientedUpdates) const final {}

    void acceptVisitor(UpdateNodeVisitor* visitor) final {
        visitor->visit(this);
    }
};

}  // namespace mongo
