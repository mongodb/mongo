// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/mutable_bson/element.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/update/modifier_node.h"
#include "mongo/db/update/update_node.h"
#include "mongo/db/update/update_node_visitor.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

/**
 * Represents the application of $max or $min to the value at the end of a path.
 */
class CompareNode : public ModifierNode {
public:
    enum class CompareMode { kMax, kMin };

    explicit CompareNode(CompareMode mode) : _mode(mode) {}

    Status init(BSONElement modExpr, const boost::intrusive_ptr<ExpressionContext>& expCtx) final;

    std::unique_ptr<UpdateNode> clone() const final {
        return std::make_unique<CompareNode>(*this);
    }

    void setCollator(const CollatorInterface* collator) final;

    void acceptVisitor(UpdateNodeVisitor* visitor) final {
        visitor->visit(this);
    }

protected:
    ModifyResult updateExistingElement(mutablebson::Element* element,
                                       const FieldRef& elementPath) const final;
    void setValueForNewElement(mutablebson::Element* element) const final;

    bool allowCreation() const final {
        return true;
    }

    bool canSetObjectValue() const final {
        return true;
    }

private:
    std::string_view operatorName() const final {
        switch (_mode) {
            case CompareMode::kMax:
                return "$max";
            case CompareMode::kMin:
                return "$min";
            default:
                MONGO_UNREACHABLE;
        }
    }

    BSONObj operatorValue(const query_shape::SerializationOptions& opts) const final {
        return BSON("" << opts.serializeLiteral(_val));
    }

    CompareMode _mode;
    BSONElement _val;
    const CollatorInterface* _collator = nullptr;
};

}  // namespace mongo
