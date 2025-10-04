/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include <boost/container/small_vector.hpp>
// IWYU pragma: no_include "boost/intrusive/detail/iterator.hpp"
#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/mutable_bson/algorithm.h"
#include "mongo/db/exec/mutable_bson/const_element.h"
#include "mongo/db/exec/mutable_bson/document.h"
#include "mongo/db/exec/mutable_bson/element.h"
#include "mongo/db/field_ref_set.h"
#include "mongo/db/update/field_checker.h"
#include "mongo/db/update/modifier_node.h"
#include "mongo/db/update/path_support.h"
#include "mongo/db/update/rename_node.h"
#include "mongo/db/update/runtime_update_path.h"
#include "mongo/db/update/unset_node.h"
#include "mongo/db/update/update_executor.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <cstddef>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

/**
 * The SetElementNode class provides the $set functionality for $rename. A $rename from a source
 * field to a destination field behaves logically like a $set on the destination followed by a
 * $unset on the source, and the first of those operations is executed by calling apply on a
 * SetElementNode object. We create a class for this purpose (rather than a stand-alone function) so
 * that it can inherit from ModifierNode.
 *
 * Unlike SetNode, SetElementNode takes a mutablebson::Element as its input. Additionally,
 * SetElementNode::updateExistingElement() does not check for the possibility that we are
 * overwriting the target value with an identical source value (a no-op). That check would require
 * us to convert _elemToSet from a mutablebson::Element to a BSONElement, which is not worth the
 * extra time.
 */
class SetElementNode : public ModifierNode {
public:
    SetElementNode(mutablebson::Element elemToSet) : _elemToSet(elemToSet) {}

    std::unique_ptr<UpdateNode> clone() const final {
        return std::make_unique<SetElementNode>(*this);
    }

    void setCollator(const CollatorInterface* collator) final {}

    Status init(BSONElement modExpr,
                const boost::intrusive_ptr<ExpressionContext>& expCtx) override {
        return Status::OK();
    }

    /**
     * These internally-generated nodes do not need to be serialized.
     */
    void produceSerializationMap(
        FieldRef* currentPath,
        std::map<std::string, std::vector<std::pair<std::string, BSONObj>>>*
            operatorOrientedUpdates) const final {}

    void acceptVisitor(UpdateNodeVisitor* visitor) final {
        visitor->visit(this);
    }

protected:
    ModifierNode::ModifyResult updateExistingElement(mutablebson::Element* element,
                                                     const FieldRef& elementPath) const final {
        invariant(element->setValueElement(_elemToSet));
        return ModifyResult::kNormalUpdate;
    }

    void setValueForNewElement(mutablebson::Element* element) const final {
        invariant(element->setValueElement(_elemToSet));
    }

    bool allowCreation() const final {
        return true;
    }

    bool canSetObjectValue() const final {
        return true;
    }

private:
    StringData operatorName() const final {
        return "$set";
    }

    BSONObj operatorValue() const final {
        return BSON("" << _elemToSet.getValue());
    }

    mutablebson::Element _elemToSet;
};

Status RenameNode::init(BSONElement modExpr,
                        const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    invariant(modExpr.ok());
    invariant(BSONType::string == modExpr.type());

    FieldRef fromFieldRef(modExpr.fieldName());
    FieldRef toFieldRef(modExpr.String());

    tassert(9867601,
            "The 'to' field for $rename cannot contain an embedded null byte",
            modExpr.valueStringData().find('\0') == std::string::npos);

    // Parsing {$rename: {'from': 'to'}} places nodes in the UpdateNode tree for both the "from" and
    // "to" paths via UpdateObjectNode::parseAndMerge(), which will enforce this isUpdatable
    // property.
    dassert(fieldchecker::isUpdatable(fromFieldRef));
    dassert(fieldchecker::isUpdatable(toFieldRef));

    // Though we could treat this as a no-op, it is illegal in the current implementation.
    if (fromFieldRef == toFieldRef) {
        return Status(ErrorCodes::BadValue,
                      str::stream()
                          << "The source and target field for $rename must differ: " << modExpr);
    }

    if (fromFieldRef.isPrefixOf(toFieldRef) || toFieldRef.isPrefixOf(fromFieldRef)) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "The source and target field for $rename must "
                                       "not be on the same path: "
                                    << modExpr);
    }

    size_t dummyPos;
    if (fieldchecker::isPositional(fromFieldRef, &dummyPos) ||
        fieldchecker::hasArrayFilter(fromFieldRef)) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "The source field for $rename may not be dynamic: "
                                    << fromFieldRef.dottedField());
    } else if (fieldchecker::isPositional(toFieldRef, &dummyPos) ||
               fieldchecker::hasArrayFilter(toFieldRef)) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "The destination field for $rename may not be dynamic: "
                                    << toFieldRef.dottedField());
    }

    _val = modExpr;

    return Status::OK();
}

UpdateExecutor::ApplyResult RenameNode::apply(ApplyParams applyParams,
                                              UpdateNodeApplyParams updateNodeApplyParams) const {
    // It would make sense to store fromFieldRef and toFieldRef as members during
    // RenameNode::init(), but FieldRef is not copyable.
    FieldRef fromFieldRef(_val.fieldName());
    FieldRef toFieldRef(_val.valueStringData());

    mutablebson::Document& document = applyParams.element.getDocument();

    FieldIndex fromIdxFound;
    mutablebson::Element fromElement(document.end());
    auto status =
        pathsupport::findLongestPrefix(fromFieldRef, document.root(), &fromIdxFound, &fromElement);

    if (!status.isOK() || !fromElement.ok() || fromIdxFound != (fromFieldRef.numParts() - 1)) {
        // We could safely remove this restriction (thereby treating a rename with a non-viable
        // source path as a no-op), but most updates fail on an attempt to update a non-viable path,
        // so we throw an error for consistency.
        if (status == ErrorCodes::PathNotViable) {
            uassertStatusOK(status);
            MONGO_UNREACHABLE;  // The previous uassertStatusOK should always throw.
        }

        // The element we want to rename does not exist. When that happens, we treat the operation
        // as a no-op. The attempted from/to paths are still considered modified.
        if (applyParams.modifiedPaths) {
            applyParams.modifiedPaths->keepShortest(fromFieldRef);
            applyParams.modifiedPaths->keepShortest(toFieldRef);
        }
        return ApplyResult::noopResult();
    }

    // Renaming through an array is prohibited. Check that our source path does not contain an
    // array. (The element being renamed may be an array, however.)
    for (auto currentElement = fromElement.parent(); currentElement != document.root();
         currentElement = currentElement.parent()) {
        invariant(currentElement.ok());
        if (BSONType::array == currentElement.getType()) {
            auto idElem = mutablebson::findFirstChildNamed(document.root(), "_id");
            uasserted(ErrorCodes::BadValue,
                      str::stream() << "The source field cannot be an array element, '"
                                    << fromFieldRef.dottedField() << "' in doc with "
                                    << (idElem.ok() ? idElem.toString() : "no id")
                                    << " has an array field called '"
                                    << currentElement.getFieldName() << "'");
        }
    }

    // Check that our destination path does not contain an array. (If the rename will overwrite an
    // existing element, that element may be an array. Iff pathToCreate is empty, "element"
    // represents an element that we are going to overwrite.)
    for (auto currentElement = updateNodeApplyParams.pathToCreate->empty()
             ? applyParams.element.parent()
             : applyParams.element;
         currentElement != document.root();
         currentElement = currentElement.parent()) {
        invariant(currentElement.ok());
        if (BSONType::array == currentElement.getType()) {
            auto idElem = mutablebson::findFirstChildNamed(document.root(), "_id");
            uasserted(ErrorCodes::BadValue,
                      str::stream() << "The destination field cannot be an array element, '"
                                    << toFieldRef.dottedField() << "' in doc with "
                                    << (idElem.ok() ? idElem.toString() : "no id")
                                    << " has an array field called '"
                                    << currentElement.getFieldName() << "'");
        }
    }

    // Once we've determined that the rename is valid and found the source element, the actual work
    // gets broken out into a $set operation and an $unset operation. Note that, generally, we
    // should call the init() method of a ModifierNode before calling its apply() method, but the
    // init() methods of SetElementNode and UnsetNode don't do anything, so we can skip them.
    SetElementNode setElement(fromElement);
    auto setElementApplyResult = setElement.apply(applyParams, updateNodeApplyParams);

    ApplyParams unsetParams(applyParams);
    unsetParams.element = fromElement;

    // Renames never "go through" arrays, so we're guaranteed all of the parts of the path are
    // of type kFieldName.
    auto pathTaken = std::make_shared<RuntimeUpdatePath>(
        fromFieldRef,
        std::vector<RuntimeUpdatePath::ComponentType>(
            fromFieldRef.numParts(), RuntimeUpdatePath::ComponentType::kFieldName));

    UpdateNodeApplyParams unsetUpdateNodeApplyParams{
        std::make_shared<FieldRef>(),
        pathTaken,
        updateNodeApplyParams.logBuilder,
    };

    UnsetNode unsetElement;
    auto unsetElementApplyResult = unsetElement.apply(unsetParams, unsetUpdateNodeApplyParams);

    // The $unset would only be a no-op if the source element did not exist, in which case we would
    // have exited early with a no-op result.
    invariant(!unsetElementApplyResult.noop);

    return {};
}

}  // namespace mongo
