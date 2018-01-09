/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/update/rename_node.h"

#include "mongo/bson/mutable/algorithm.h"
#include "mongo/db/update/field_checker.h"
#include "mongo/db/update/modifier_node.h"
#include "mongo/db/update/path_support.h"
#include "mongo/db/update/storage_validation.h"
#include "mongo/db/update/unset_node.h"

namespace mongo {

namespace {

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
        return stdx::make_unique<SetElementNode>(*this);
    }

    void setCollator(const CollatorInterface* collator) final {}

    Status init(BSONElement modExpr, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
        return Status::OK();
    }

protected:
    ModifierNode::ModifyResult updateExistingElement(
        mutablebson::Element* element, std::shared_ptr<FieldRef> elementPath) const final {
        invariantOK(element->setValueElement(_elemToSet));
        return ModifyResult::kNormalUpdate;
    }

    void setValueForNewElement(mutablebson::Element* element) const final {
        invariantOK(element->setValueElement(_elemToSet));
    }

    bool allowCreation() const final {
        return true;
    }

    bool canSetObjectValue() const final {
        return true;
    }

private:
    mutablebson::Element _elemToSet;
};

}  // namespace

Status RenameNode::init(BSONElement modExpr,
                        const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    invariant(modExpr.ok());
    invariant(BSONType::String == modExpr.type());

    FieldRef fromFieldRef(modExpr.fieldName());
    FieldRef toFieldRef(modExpr.String());

    if (modExpr.valueStringData().find('\0') != std::string::npos) {
        return Status(ErrorCodes::BadValue,
                      "The 'to' field for $rename cannot contain an embedded null byte");
    }

    // Parsing {$rename: {'from': 'to'}} places nodes in the UpdateNode tree for both the "from" and
    // "to" paths via UpdateObjectNode::parseAndMerge(), which will enforce this isUpdatable
    // property.
    dassert(fieldchecker::isUpdatable(fromFieldRef).isOK());
    dassert(fieldchecker::isUpdatable(toFieldRef).isOK());

    // Though we could treat this as a no-op, it is illegal in the current implementation.
    if (fromFieldRef == toFieldRef) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "The source and target field for $rename must differ: "
                                    << modExpr);
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

UpdateNode::ApplyResult RenameNode::apply(ApplyParams applyParams) const {
    // It would make sense to store fromFieldRef and toFieldRef as members during
    // RenameNode::init(), but FieldRef is not copyable.
    auto fromFieldRef = std::make_shared<FieldRef>(_val.fieldName());
    FieldRef toFieldRef(_val.valueStringData());

    mutablebson::Document& document = applyParams.element.getDocument();

    size_t fromIdxFound;
    mutablebson::Element fromElement(document.end());
    auto status =
        pathsupport::findLongestPrefix(*fromFieldRef, document.root(), &fromIdxFound, &fromElement);

    if (!status.isOK() || !fromElement.ok() || fromIdxFound != (fromFieldRef->numParts() - 1)) {
        // We could safely remove this restriction (thereby treating a rename with a non-viable
        // source path as a no-op), but most updates fail on an attempt to update a non-viable path,
        // so we throw an error for consistency.
        if (status == ErrorCodes::PathNotViable) {
            uassertStatusOK(status);
            MONGO_UNREACHABLE;  // The previous uassertStatusOK should always throw.
        }

        // The element we want to rename does not exist. When that happens, we treat the operation
        // as a no-op.
        return ApplyResult::noopResult();
    }

    // Renaming through an array is prohibited. Check that our source path does not contain an
    // array. (The element being renamed may be an array, however.)
    for (auto currentElement = fromElement.parent(); currentElement != document.root();
         currentElement = currentElement.parent()) {
        invariant(currentElement.ok());
        if (BSONType::Array == currentElement.getType()) {
            auto idElem = mutablebson::findFirstChildNamed(document.root(), "_id");
            uasserted(ErrorCodes::BadValue,
                      str::stream() << "The source field cannot be an array element, '"
                                    << fromFieldRef->dottedField()
                                    << "' in doc with "
                                    << (idElem.ok() ? idElem.toString() : "no id")
                                    << " has an array field called '"
                                    << currentElement.getFieldName()
                                    << "'");
        }
    }

    // Check that our destination path does not contain an array. (If the rename will overwrite an
    // existing element, that element may be an array. Iff pathToCreate is empty, "element"
    // represents an element that we are going to overwrite.)
    for (auto currentElement = applyParams.pathToCreate->empty() ? applyParams.element.parent()
                                                                 : applyParams.element;
         currentElement != document.root();
         currentElement = currentElement.parent()) {
        invariant(currentElement.ok());
        if (BSONType::Array == currentElement.getType()) {
            auto idElem = mutablebson::findFirstChildNamed(document.root(), "_id");
            uasserted(ErrorCodes::BadValue,
                      str::stream() << "The destination field cannot be an array element, '"
                                    << toFieldRef.dottedField()
                                    << "' in doc with "
                                    << (idElem.ok() ? idElem.toString() : "no id")
                                    << " has an array field called '"
                                    << currentElement.getFieldName()
                                    << "'");
        }
    }

    // Once we've determined that the rename is valid and found the source element, the actual work
    // gets broken out into a $set operation and an $unset operation. Note that, generally, we
    // should call the init() method of a ModifierNode before calling its apply() method, but the
    // init() methods of SetElementNode and UnsetNode don't do anything, so we can skip them.
    SetElementNode setElement(fromElement);
    auto setElementApplyResult = setElement.apply(applyParams);

    ApplyParams unsetParams(applyParams);
    unsetParams.element = fromElement;
    unsetParams.pathToCreate = std::make_shared<FieldRef>();
    unsetParams.pathTaken = fromFieldRef;

    UnsetNode unsetElement;
    auto unsetElementApplyResult = unsetElement.apply(unsetParams);

    // Determine the final result based on the results of the $set and $unset.
    ApplyResult applyResult;
    applyResult.indexesAffected =
        setElementApplyResult.indexesAffected || unsetElementApplyResult.indexesAffected;

    // The $unset would only be a no-op if the source element did not exist, in which case we would
    // have exited early with a no-op result.
    invariant(!unsetElementApplyResult.noop);

    return applyResult;
}

}  // namespace mongo
