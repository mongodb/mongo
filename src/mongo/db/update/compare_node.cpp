// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/update/compare_node.h"

#include "mongo/db/query/collation/collator_interface.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

Status CompareNode::init(BSONElement modExpr,
                         const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    invariant(modExpr.ok());
    _val = modExpr;
    setCollator(expCtx->getCollator());
    return Status::OK();
}

void CompareNode::setCollator(const CollatorInterface* collator) {
    invariant(!_collator);
    _collator = collator;
}

ModifierNode::ModifyResult CompareNode::updateExistingElement(mutablebson::Element* element,
                                                              const FieldRef& elementPath) const {
    const auto compareVal = element->compareWithBSONElement(_val, _collator, false);
    if ((compareVal == 0) || ((_mode == CompareMode::kMax) ? (compareVal > 0) : (compareVal < 0))) {
        return ModifyResult::kNoOp;
    } else {
        invariant(element->setValueBSONElement(_val));
        return ModifyResult::kNormalUpdate;
    }
}

void CompareNode::setValueForNewElement(mutablebson::Element* element) const {
    invariant(element->setValueBSONElement(_val));
}

}  // namespace mongo
