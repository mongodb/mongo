// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/update/update_leaf_node.h"
#include "mongo/db/update/update_node.h"
#include "mongo/db/update/update_node_visitor.h"
#include "mongo/util/modules.h"

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

/**
 * Represents the application of a $rename to the value at the end of a path, where the path
 * represents the rename destination.
 */
class RenameNode : public UpdateLeafNode {
public:
    /**
     * This init provides input validation on the source field (stored as the field name in
     * "modExpr") and the destination field (stored as the value in "modExpr").
     */
    Status init(BSONElement modExpr, const boost::intrusive_ptr<ExpressionContext>& expCtx) final;

    std::unique_ptr<UpdateNode> clone() const final {
        return std::make_unique<RenameNode>(*this);
    }

    void setCollator(const CollatorInterface* collator) final {}

    ApplyResult apply(ApplyParams applyParams,
                      UpdateNodeApplyParams updateNodeApplyParams) const final;

    void produceSerializationMap(
        FieldRef* currentPath,
        std::map<std::string, std::vector<std::pair<std::string, BSONObj>>>*
            operatorOrientedUpdates,
        const query_shape::SerializationOptions& opts) const final {
        // The RenameNode sits in the update tree at the destination path, because that's the path
        // that may need to be synthesized if it's not already in the document. However, the
        // destination path is the _value_ and goes on the right side of the rename element (i.e.:
        // {$rename: {sourcePath: "destPath"}}), unlike all other modifiers, where the path to
        // synthesize is the field (on the left).

        (*operatorOrientedUpdates)["$rename"].emplace_back(
            opts.serializeFieldPathFromString(_val.fieldName()),
            BSON("" << opts.serializeFieldRef(*currentPath)));
    }

    void acceptVisitor(UpdateNodeVisitor* visitor) final {
        visitor->visit(this);
    }

    BSONElement getValue() const {
        return _val;
    }

private:
    BSONElement _val;
};

}  // namespace mongo
