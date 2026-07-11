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
 * Represents the application of a $pull to the value at the end of a path.
 *
 * The $pull update modifier updates an array by removing all values that match the supplied
 * condition. If the value passed to the $pull modifier is a primitive value or array, then only
 * exact matches are removed.
 */
class PullNode final : public ArrayCullingNode {
public:
    Status init(BSONElement modExpr, const boost::intrusive_ptr<ExpressionContext>& expCtx) final;

    std::unique_ptr<UpdateNode> clone() const final {
        return std::make_unique<PullNode>(*this);
    }

    void acceptVisitor(UpdateNodeVisitor* visitor) final {
        visitor->visit(this);
    }

private:
    std::string_view operatorName() const final {
        return "$pull";
    }

    class ObjectMatcher;
    class WrappedObjectMatcher;
    class EqualityMatcher;
};

}  // namespace mongo
