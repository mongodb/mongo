// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/update/set_node.h"

#include "mongo/util/assert_util.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

Status SetNode::init(BSONElement modExpr, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    invariant(modExpr.ok());

    val = modExpr;

    return Status::OK();
}

ModifierNode::ModifyResult SetNode::updateExistingElement(mutablebson::Element* element,
                                                          const FieldRef& elementPath) const {
    // If 'element' is deserialized, then element.getValue() will be EOO, which will never equal
    // val.
    if (element->getValue().binaryEqualValues(val)) {
        return ModifyResult::kNoOp;
    } else {
        invariant(element->setValueBSONElement(val));
        return ModifyResult::kNormalUpdate;
    }
}

void SetNode::setValueForNewElement(mutablebson::Element* element) const {
    invariant(element->setValueBSONElement(val));
}

}  // namespace mongo
