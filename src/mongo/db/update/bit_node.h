// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/mutable_bson/element.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/update/modifier_node.h"
#include "mongo/db/update/update_node.h"
#include "mongo/db/update/update_node_visitor.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"
#include "mongo/util/safe_num.h"

#include <memory>
#include <string_view>
#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

/**
 * Represents the application of a $bit to the value at the end of a path.
 */
class BitNode : public ModifierNode {
public:
    Status init(BSONElement modExpr, const boost::intrusive_ptr<ExpressionContext>& expCtx) final;

    std::unique_ptr<UpdateNode> clone() const final {
        return std::make_unique<BitNode>(*this);
    }

    void setCollator(const CollatorInterface* collator) final {}

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

private:
    std::string_view operatorName() const final {
        return "$bit";
    }

    BSONObj operatorValue(const query_shape::SerializationOptions& opts) const final {
        BSONObjBuilder bob;
        {
            BSONObjBuilder subBuilder(bob.subobjStart(""));
            for (const auto& [bitOperator, operand] : _opList) {
                std::string operandName;
                if (bitOperator == &SafeNum::bitAnd)
                    operandName = "and";
                else if (bitOperator == &SafeNum::bitOr)
                    operandName = "or";
                else if (bitOperator == &SafeNum::bitXor)
                    operandName = "xor";
                else
                    MONGO_UNREACHABLE_TASSERT(11034400);

                if (opts.isDefaultSerialization()) {
                    operand.toBSON(operandName, &subBuilder);
                } else {
                    // Serialize dummy numeric value.
                    // TODO SERVER-114855: Ideally, we should pass in
                    // query_shape::SerializationOptions to the SafeNum::toBSON(...) and handle
                    // serialization inside there. Unfortunately, there is a circular dependency
                    // between both of those libraries.
                    subBuilder << operandName << opts.serializeLiteral(1);
                }
            }
        }
        return bob.obj();
    }

    /**
     * Applies each op in "_opList" to "value" and returns the result.
     */
    SafeNum applyOpList(SafeNum value) const;

    struct BitwiseOp {
        SafeNum (SafeNum::*bitOperator)(const SafeNum&) const;
        SafeNum operand;
    };

    std::vector<BitwiseOp> _opList;
};

}  // namespace mongo
