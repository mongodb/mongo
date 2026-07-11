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
#include "mongo/db/update/modifier_node.h"
#include "mongo/db/update/update_node.h"
#include "mongo/db/update/update_node_visitor.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <memory>
#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

class PopNode final : public ModifierNode {
public:
    Status init(BSONElement modExpr, const boost::intrusive_ptr<ExpressionContext>& expCtx) final;

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

    std::unique_ptr<UpdateNode> clone() const final {
        return std::make_unique<PopNode>(*this);
    }

    void setCollator(const CollatorInterface* collator) final {}

    /**
     * Returns true if this node is popping the first element off the front of the array. Returns
     * false if this node is popping the last element off the back of the array.
     */
    bool popFromFront() const {
        return _popFromFront;
    }

    void acceptVisitor(UpdateNodeVisitor* visitor) final {
        visitor->visit(this);
    }

private:
    std::string_view operatorName() const final {
        return "$pop";
    }

    BSONObj operatorValue(const query_shape::SerializationOptions& opts) const final {
        // Since the only valid values for $pop are 1 and -1, this is more of an enum than an actual
        // user data value. Thus, we can directly return the value here.
        return BSON("" << (_popFromFront ? -1 : 1));
    }

    bool _popFromFront = true;
};

}  // namespace mongo
