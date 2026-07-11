// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/update/pop_node.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/update/storage_validation.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

Status PopNode::init(BSONElement modExpr, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    auto popVal = modExpr.parseIntegerElementToLong();
    if (!popVal.isOK()) {
        return popVal.getStatus();
    }
    if (popVal.getValue() != 1LL && popVal.getValue() != -1LL) {
        return {ErrorCodes::FailedToParse,
                str::stream() << "$pop expects 1 or -1, found: " << popVal.getValue()};
    }
    _popFromFront = (popVal.getValue() == -1LL);
    return Status::OK();
}

ModifierNode::ModifyResult PopNode::updateExistingElement(mutablebson::Element* element,
                                                          const FieldRef& elementPath) const {
    invariant(element->ok());
    uassert(ErrorCodes::TypeMismatch,
            str::stream() << "Path '" << elementPath.dottedField()
                          << "' contains an element of non-array type '"
                          << typeName(element->getType()) << "'",
            element->getType() == BSONType::array);

    if (!element->hasChildren()) {
        // The path exists and contains an array, but the array is empty.
        return ModifyResult::kNoOp;
    }

    auto elementToRemove = _popFromFront ? element->leftChild() : element->rightChild();
    invariant(elementToRemove.remove());

    return ModifyResult::kNormalUpdate;
}

void PopNode::validateUpdate(mutablebson::ConstElement updatedElement,
                             mutablebson::ConstElement leftSibling,
                             mutablebson::ConstElement rightSibling,
                             std::uint32_t recursionLevel,
                             ModifyResult modifyResult,
                             const bool validateForStorage,
                             bool* containsDotsAndDollarsField,
                             const bool fromOplogApplication) const {
    invariant(modifyResult.type == ModifyResult::kNormalUpdate);

    // Removing elements from an array cannot increase BSON depth or modify a DBRef, so we can
    // override validateUpdate to not validate storage constraints but we still want to know if
    // there is any field name containing '.'/'$'.
    bool doRecursiveCheck = true;
    storage_validation::scanDocument(updatedElement,
                                     doRecursiveCheck,
                                     recursionLevel,
                                     false, /* allowTopLevelDollarPrefixedFields */
                                     false, /* Should validate for storage */
                                     false, /* isEmbeddedInIdField */
                                     containsDotsAndDollarsField,
                                     fromOplogApplication);
}

}  // namespace mongo
