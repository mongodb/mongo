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

#include "mongo/db/update/modifier_node.h"

#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/db/update/path_support.h"
#include "mongo/db/update/storage_validation.h"

namespace mongo {

namespace {

/**
 * Checks that no immutable paths were modified in the case where we are modifying an existing path
 * in the document.
 *
 * This check does not make assumptions about how 'element' was modified; it explicitly checks
 * immutable fields in 'element' to see if they differ from the 'original' value. Consider an
 * updating the document {a: {b: 1, c: 1}} with {$set: {a: {b: 1, d: 1}}} where 'a.b' is an
 * immutable path. Even though we've overwritten the immutable field, it has the same value, and the
 * update is allowed.
 *
 * 'element' should be the modified element. 'pathTaken' is the path to the modified element.
 * 'original' should be provided as the preimage of the whole document. We _do_ assume that we have
 * already checked the update is not a noop.
 */
void checkImmutablePathsNotModifiedFromOriginal(mutablebson::Element element,
                                                FieldRef* pathTaken,
                                                const FieldRefSet& immutablePaths,
                                                BSONObj original) {
    for (auto immutablePath = immutablePaths.begin(); immutablePath != immutablePaths.end();
         ++immutablePath) {
        auto prefixSize = pathTaken->commonPrefixSize(**immutablePath);

        // If 'immutablePath' is a (strict or non-strict) prefix of 'pathTaken', and the update is
        // not a noop, then we have modified 'immutablePath', which is immutable.
        if (prefixSize == (*immutablePath)->numParts()) {
            uasserted(ErrorCodes::ImmutableField,
                      str::stream() << "Updating the path '" << pathTaken->dottedField() << "' to "
                                    << element.toString()
                                    << " would modify the immutable field '"
                                    << (*immutablePath)->dottedField()
                                    << "'");
        }

        // If 'pathTaken' is a strict prefix of 'immutablePath', then we may have modified
        // 'immutablePath'. We already know that 'pathTaken' is not equal to 'immutablePath', or we
        // would have uasserted.
        if (prefixSize == pathTaken->numParts()) {
            auto oldElem = dotted_path_support::extractElementAtPath(
                original, (*immutablePath)->dottedField());

            // We are allowed to modify immutable paths that do not yet exist.
            if (!oldElem.ok()) {
                continue;
            }

            auto newElem = element;
            for (size_t i = pathTaken->numParts(); i < (*immutablePath)->numParts(); ++i) {
                uassert(ErrorCodes::NotSingleValueField,
                        str::stream()
                            << "After applying the update to the document, the immutable field '"
                            << (*immutablePath)->dottedField()
                            << "' was found to be an array or array descendant.",
                        newElem.getType() != BSONType::Array);
                newElem = newElem[(*immutablePath)->getPart(i)];
                if (!newElem.ok()) {
                    break;
                }
            }

            uassert(ErrorCodes::ImmutableField,
                    str::stream() << "After applying the update, the immutable field '"
                                  << (*immutablePath)->dottedField()
                                  << "' was found to have been removed.",
                    newElem.ok());
            uassert(ErrorCodes::ImmutableField,
                    str::stream() << "After applying the update, the immutable field '"
                                  << (*immutablePath)->dottedField()
                                  << "' was found to have been altered to "
                                  << newElem.toString(),
                    newElem.compareWithBSONElement(oldElem, nullptr, false) == 0);
        }
    }
}

/**
 * Checks that no immutable paths were modified in the case where we are modifying an existing path
 * in the document.
 *
 * Unlike checkImmutablePathsNotModifiedFromOriginal(), this function does not check the original
 * document. It always assumes that an update where 'pathTaken' is a prefix of any immutable path or
 * vice versa is modifying an immutable path. This assumption is valid when we already know that
 * 'pathTaken' is not a prefix of any immutable path or when the update is to a primitive value or
 * array. (Immutable paths cannot include array elements.)
 *
 * See the comment above checkImmutablePathNotModifiedFromOriginal() for an example where that
 * assumption does not apply.
 *
 * 'element' should be the modified element. 'pathTaken' is the path to the modified element. We
 * assume that we have already checked the update is not a noop.
 */
void checkImmutablePathsNotModified(mutablebson::Element element,
                                    FieldRef* pathTaken,
                                    const FieldRefSet& immutablePaths) {
    for (auto immutablePath = immutablePaths.begin(); immutablePath != immutablePaths.end();
         ++immutablePath) {
        uassert(ErrorCodes::ImmutableField,
                str::stream() << "Performing an update on the path '" << pathTaken->dottedField()
                              << "' would modify the immutable field '"
                              << (*immutablePath)->dottedField()
                              << "'",
                pathTaken->commonPrefixSize(**immutablePath) <
                    std::min(pathTaken->numParts(), (*immutablePath)->numParts()));
    }
}

}  // namespace

UpdateNode::ApplyResult ModifierNode::applyToExistingElement(ApplyParams applyParams) const {
    invariant(!applyParams.pathTaken->empty());
    invariant(applyParams.pathToCreate->empty());
    invariant(applyParams.element.ok());

    mutablebson::ConstElement leftSibling = applyParams.element.leftSibling();
    mutablebson::ConstElement rightSibling = applyParams.element.rightSibling();

    bool compareWithOriginal = false;
    if (canSetObjectValue()) {
        for (auto immutablePath = applyParams.immutablePaths.begin();
             immutablePath != applyParams.immutablePaths.end();
             ++immutablePath) {
            if (applyParams.pathTaken->isPrefixOf(**immutablePath)) {
                compareWithOriginal = true;
                break;
            }
        }
    }

    // We have two different ways of checking for changes to immutable paths, depending on the style
    // of update. See the comments above checkImmutablePathsNotModifiedFromOriginal() and
    // checkImmutablePathsNotModified().
    ModifyResult updateResult;
    if (compareWithOriginal) {
        BSONObj original = applyParams.element.getDocument().getObject();
        updateResult = updateExistingElement(&applyParams.element, applyParams.pathTaken);
        if (updateResult == ModifyResult::kNoOp) {
            return ApplyResult::noopResult();
        }
        checkImmutablePathsNotModifiedFromOriginal(
            applyParams.element, applyParams.pathTaken.get(), applyParams.immutablePaths, original);
    } else {
        updateResult = updateExistingElement(&applyParams.element, applyParams.pathTaken);
        if (updateResult == ModifyResult::kNoOp) {
            return ApplyResult::noopResult();
        }
        checkImmutablePathsNotModified(
            applyParams.element, applyParams.pathTaken.get(), applyParams.immutablePaths);
    }
    invariant(updateResult != ModifyResult::kCreated);

    ApplyResult applyResult;

    if (!applyParams.indexData ||
        !applyParams.indexData->mightBeIndexed(applyParams.pathTaken->dottedField())) {
        applyResult.indexesAffected = false;
    }

    if (applyParams.validateForStorage) {
        const uint32_t recursionLevel = applyParams.pathTaken->numParts();
        validateUpdate(
            applyParams.element, leftSibling, rightSibling, recursionLevel, updateResult);
    }

    if (applyParams.logBuilder) {
        logUpdate(applyParams.logBuilder,
                  applyParams.pathTaken->dottedField(),
                  applyParams.element,
                  updateResult);
    }

    return applyResult;
}

UpdateNode::ApplyResult ModifierNode::applyToNonexistentElement(ApplyParams applyParams) const {
    if (allowCreation()) {
        auto newElementFieldName =
            applyParams.pathToCreate->getPart(applyParams.pathToCreate->numParts() - 1);
        auto newElement = applyParams.element.getDocument().makeElementNull(newElementFieldName);
        setValueForNewElement(&newElement);

        invariant(newElement.ok());
        auto statusWithFirstCreatedElem = pathsupport::createPathAt(
            *(applyParams.pathToCreate), 0, applyParams.element, newElement);
        if (!statusWithFirstCreatedElem.isOK()) {
            // $set operaions on non-viable paths are ignored when the update came from replication.
            // We do not error because idempotency requires that any other update modifiers must
            // still be applied. For example, consider applying the following updates twice to an
            // initially empty document:
            // {$set: {c: 0}}
            // {$set: {'a.b': 0, c: 1}}
            // {$set: {a: 0}}
            // Setting 'a.b' will fail the second time, but we must still set 'c'.
            // (There are modifiers besides $set that use this code path, but they are not used for
            // replication, so we are not concerned with their behavior when "fromOplogApplication"
            // is true.)
            if (statusWithFirstCreatedElem.getStatus().code() == ErrorCodes::PathNotViable &&
                applyParams.fromOplogApplication) {
                return ApplyResult::noopResult();
            }
            uassertStatusOK(statusWithFirstCreatedElem);
            MONGO_UNREACHABLE;  // The previous uassertStatusOK should always throw.
        }

        if (applyParams.validateForStorage) {
            const uint32_t recursionLevel = applyParams.pathTaken->numParts() + 1;
            mutablebson::ConstElement elementForValidation = statusWithFirstCreatedElem.getValue();
            validateUpdate(elementForValidation,
                           elementForValidation.leftSibling(),
                           elementForValidation.rightSibling(),
                           recursionLevel,
                           ModifyResult::kCreated);
        }

        for (auto immutablePath = applyParams.immutablePaths.begin();
             immutablePath != applyParams.immutablePaths.end();
             ++immutablePath) {

            // If 'immutablePath' is a (strict or non-strict) prefix of 'pathTaken', then we are
            // modifying 'immutablePath'. For example, adding '_id.x' will illegally modify '_id'.
            // (Note that this behavior is subtly different from checkImmutablePathsNotModified(),
            // because we just created this element.)
            uassert(ErrorCodes::ImmutableField,
                    str::stream() << "Updating the path '" << applyParams.pathTaken->dottedField()
                                  << "' to "
                                  << applyParams.element.toString()
                                  << " would modify the immutable field '"
                                  << (*immutablePath)->dottedField()
                                  << "'",
                    applyParams.pathTaken->commonPrefixSize(**immutablePath) !=
                        (*immutablePath)->numParts());
        }

        invariant(!applyParams.pathToCreate->empty());
        std::string fullPath;
        if (applyParams.pathTaken->empty()) {
            fullPath = applyParams.pathToCreate->dottedField().toString();
        } else {
            fullPath = str::stream() << applyParams.pathTaken->dottedField() << "."
                                     << applyParams.pathToCreate->dottedField();
        }

        ApplyResult applyResult;

        // Determine if indexes are affected. If we did not create a new element in an array, check
        // whether the full path affects indexes. If we did create a new element in an array, check
        // whether the array itself might affect any indexes. This is necessary because if there is
        // an index {"a.b": 1}, and we set "a.1.c" and implicitly create an array element in "a",
        // then we may need to add a null key to the index, even though "a.1.c" does not appear to
        // affect the index.
        if (!applyParams.indexData ||
            !applyParams.indexData->mightBeIndexed(applyParams.element.getType() != BSONType::Array
                                                       ? fullPath
                                                       : applyParams.pathTaken->dottedField())) {
            applyResult.indexesAffected = false;
        }

        if (applyParams.logBuilder) {
            logUpdate(applyParams.logBuilder, fullPath, newElement, ModifyResult::kCreated);
        }

        return applyResult;
    } else {
        // This path is for modifiers like $pop or $pull that generally have no effect when applied
        // to a path that does not exist.
        if (!allowNonViablePath()) {
            // One exception: some of these modifiers still fail when the nonexistent path is
            // "non-viable," meaning it couldn't be created even if we intended to.
            UpdateLeafNode::checkViability(
                applyParams.element, *(applyParams.pathToCreate), *(applyParams.pathTaken));
        }

        return ApplyResult::noopResult();
    }
}

UpdateNode::ApplyResult ModifierNode::apply(ApplyParams applyParams) const {
    if (context == Context::kInsertOnly && !applyParams.insert) {
        return ApplyResult::noopResult();
    } else if (!applyParams.pathToCreate->empty()) {
        return applyToNonexistentElement(applyParams);
    } else {
        return applyToExistingElement(applyParams);
    }
}

void ModifierNode::validateUpdate(mutablebson::ConstElement updatedElement,
                                  mutablebson::ConstElement leftSibling,
                                  mutablebson::ConstElement rightSibling,
                                  std::uint32_t recursionLevel,
                                  ModifyResult modifyResult) const {
    const bool doRecursiveCheck = true;
    storage_validation::storageValid(updatedElement, doRecursiveCheck, recursionLevel);
}

void ModifierNode::logUpdate(LogBuilder* logBuilder,
                             StringData pathTaken,
                             mutablebson::Element element,
                             ModifyResult modifyResult) const {
    invariant(logBuilder);
    invariant(modifyResult == ModifyResult::kNormalUpdate ||
              modifyResult == ModifyResult::kCreated);
    uassertStatusOK(logBuilder->addToSetsWithNewFieldName(pathTaken, element));
}

}  // namespace mongo
