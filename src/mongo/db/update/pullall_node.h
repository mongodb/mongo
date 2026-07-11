// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/update/array_culling_node.h"
#include "mongo/db/update/update_node.h"
#include "mongo/db/update/update_node_visitor.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

/**
 * Represents the application of a $pullAll to the value at the end of a path.
 *
 * The $pullAll update modifier updates an array by removing any array elements that are an exact
 * match for one of the supplied values.
 */

class PullAllNode final : public ArrayCullingNode {
public:
    Status init(BSONElement modExpr, const boost::intrusive_ptr<ExpressionContext>& expCtx) final;

    std::unique_ptr<UpdateNode> clone() const final {
        return std::make_unique<PullAllNode>(*this);
    }

    void acceptVisitor(UpdateNodeVisitor* visitor) final {
        visitor->visit(this);
    }

private:
    std::string_view operatorName() const final {
        return "$pullAll";
    }

    /**
     * An implementation of ArrayCullingNode::ElementMatcher whose match() function returns true iff
     * its input element is exactly equal to any element from a set of candidate elements.
     */
    class SetMatcher;
};

}  // namespace mongo
