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
#include "mongo/db/update/path_creating_node.h"
#include "mongo/db/update/path_support.h"
#include "mongo/db/update/storage_validation.h"

namespace mongo {

namespace {

/**
 * The SetElementNode class provides the $set functionality for $rename. A $rename from a source
 * field to a destination field behaves logically like a $set on the destination followed by a
 * $unset on the source, and the first of those operations is executed by calling apply on a
 * SetElementNode object. We create a class for this purpose (rather than a stand-alone function) so
 * that it can inherit from PathCreatingNode.
 *
 * Unlike SetNode, SetElementNode takes a mutablebson::Element as its input.
 */
class SetElementNode : public PathCreatingNode {
public:
    SetElementNode(mutablebson::Element elemToSet) : _elemToSet(elemToSet) {}

    std::unique_ptr<UpdateNode> clone() const final {
        return stdx::make_unique<SetElementNode>(*this);
    }

    void setCollator(const CollatorInterface* collator) final {}

    Status init(BSONElement modExpr, const CollatorInterface* collator) {
        return Status::OK();
    }

    UpdateExistingElementResult updateExistingElement(mutablebson::Element* element,
                                                      std::shared_ptr<FieldRef> elementPath,
                                                      LogBuilder* logBuilder) const final {
        // In the case of a $rename where the source and destination have the same value, (e.g., we
        // are applying {$rename: {a: b}} to the document {a: "foo", b: "foo"}), there's no need to
        // modify the destination element. However, the source and destination values must be
        // _exactly_ the same, which is why we do not use collation for this check.
        StringData::ComparatorInterface* comparator = nullptr;
        auto considerFieldName = false;
        if (_elemToSet.compareWithElement(*element, comparator, considerFieldName) != 0) {
            invariantOK(element->setValueElement(_elemToSet));
            return UpdateExistingElementResult::kUpdated;
        } else {
            return UpdateExistingElementResult::kNoOp;
        }
    }

    void setValueForNewElement(mutablebson::Element* element) const final {
        invariantOK(element->setValueElement(_elemToSet));
    }

private:
    mutablebson::Element _elemToSet;
};

}  // namespace

Status RenameNode::init(BSONElement modExpr, const CollatorInterface* collator) {
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
    FieldRef fromFieldRef(_val.fieldName());
    FieldRef toFieldRef(_val.valueStringData());

    mutablebson::Document& document = applyParams.element.getDocument();

    size_t fromIdxFound;
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
                                    << fromFieldRef.dottedField()
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

    SetElementNode setElement(fromElement);
    auto setElementApplyResult = setElement.apply(applyParams);

    auto leftSibling = fromElement.leftSibling();
    auto rightSibling = fromElement.rightSibling();

    invariant(fromElement.parent().ok());
    invariantOK(fromElement.remove());

    ApplyResult applyResult;

    if (!applyParams.indexData ||
        (!setElementApplyResult.indexesAffected &&
         !applyParams.indexData->mightBeIndexed(fromFieldRef.dottedField()))) {
        applyResult.indexesAffected = false;
    }

    if (applyParams.validateForStorage) {

        // Validate the left and right sibling, in case this element was part of a DBRef.
        if (leftSibling.ok()) {
            const bool doRecursiveCheck = false;
            const uint32_t recursionLevel = 0;
            storage_validation::storageValid(leftSibling, doRecursiveCheck, recursionLevel);
        }

        if (rightSibling.ok()) {
            const bool doRecursiveCheck = false;
            const uint32_t recursionLevel = 0;
            storage_validation::storageValid(rightSibling, doRecursiveCheck, recursionLevel);
        }
    }

    // Ensure we are not changing any immutable paths.
    for (auto immutablePath = applyParams.immutablePaths.begin();
         immutablePath != applyParams.immutablePaths.end();
         ++immutablePath) {
        uassert(ErrorCodes::ImmutableField,
                str::stream() << "Unsetting the path '" << fromFieldRef.dottedField()
                              << "' using $rename would modify the immutable field '"
                              << (*immutablePath)->dottedField()
                              << "'",
                fromFieldRef.commonPrefixSize(**immutablePath) <
                    std::min(fromFieldRef.numParts(), (*immutablePath)->numParts()));
    }

    // Log the $unset. The $set was already logged by SetElementNode::apply().
    if (applyParams.logBuilder) {
        uassertStatusOK(applyParams.logBuilder->addToUnsets(fromFieldRef.dottedField()));
    }

    return applyResult;
}

}  // namespace mongo
