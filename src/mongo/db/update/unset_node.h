// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/mutable_bson/const_element.h"
#include "mongo/db/exec/mutable_bson/element.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/update/log_builder_interface.h"
#include "mongo/db/update/modifier_node.h"
#include "mongo/db/update/runtime_update_path.h"
#include "mongo/db/update/update_node.h"
#include "mongo/db/update/update_node_visitor.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <memory>
#include <string_view>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

/**
 * Represents the application of a $unset to the value at the end of a path.
 */
class UnsetNode : public ModifierNode {
public:
    Status init(BSONElement modExpr, const boost::intrusive_ptr<ExpressionContext>& expCtx) final;

    std::unique_ptr<UpdateNode> clone() const final {
        return std::make_unique<UnsetNode>(*this);
    }

    void setCollator(const CollatorInterface* collator) final {}

    ModifyResult updateExistingElement(mutablebson::Element* element,
                                       const FieldRef& elementPath) const final;

    void validateUpdate(mutablebson::ConstElement updatedElement,
                        mutablebson::ConstElement leftSibling,
                        mutablebson::ConstElement rightSibling,
                        std::uint32_t recursionLevel,
                        ModifyResult modifyResult,
                        bool validateForStorage,
                        bool* containsDotsAndDollarsField,
                        bool fromOplogApplication) const final;

    void logUpdate(LogBuilderInterface* logBuilder,
                   const RuntimeUpdatePath& pathTaken,
                   mutablebson::Element element,
                   ModifyResult modifyResult,
                   boost::optional<int> createdFieldIdx) const final;

    bool allowNonViablePath() const final {
        return true;
    }

    void acceptVisitor(UpdateNodeVisitor* visitor) final {
        visitor->visit(this);
    }

private:
    std::string_view operatorName() const final {
        return "$unset";
    }

    BSONObj operatorValue(const query_shape::SerializationOptions& opts) const final {
        // Note that the value of $unset set by user is ignored and not stored in the update tree,
        // so it is currently serialized as a constant 1. We should investigate if we should allow
        // users to specify non-1 inputs. Perhaps we should track all the non-1 values through query
        // shape stats and be serialized here.
        return BSON("" << 1);
    }
};

}  // namespace mongo
