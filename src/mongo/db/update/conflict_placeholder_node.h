// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/update/update_leaf_node.h"
#include "mongo/util/modules.h"

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
            operatorOrientedUpdates,
        const query_shape::SerializationOptions& opts) const final {}

    void acceptVisitor(UpdateNodeVisitor* visitor) final {
        visitor->visit(this);
    }
};

}  // namespace mongo
