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

#include "mongo/db/update/path_creating_node.h"

#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/db/update/path_support.h"
#include "mongo/db/update/storage_validation.h"

namespace mongo {

namespace {

/**
 * Checks that no immutable paths were modified in the case where we are modifying an existing path
 * in the document. 'element' should be the modified element. 'pathTaken' is the path to the
 * modified element. If 'pathTaken' is a strict prefix of any immutable path, 'original' should be
 * provided as the preimage of the whole document. We assume that we have already checked the update
 * is not a noop.
 */
void checkImmutablePathsNotModified(mutablebson::Element element,
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

}  // namespace

void PathCreatingNode::apply(mutablebson::Element element,
                             FieldRef* pathToCreate,
                             FieldRef* pathTaken,
                             StringData matchedField,
                             bool fromReplication,
                             bool validateForStorage,
                             const FieldRefSet& immutablePaths,
                             const UpdateIndexData* indexData,
                             LogBuilder* logBuilder,
                             bool* indexesAffected,
                             bool* noop) const {
    *indexesAffected = false;
    *noop = false;

    // The value in this Element gets used to create a logging entry (if we have a LogBuilder).
    mutablebson::Element valueToLog = element.getDocument().end();

    if (pathToCreate->empty()) {

        // If 'pathTaken' is a strict prefix of any immutable path, store the original document to
        // ensure the immutable path does not change.
        BSONObj original;
        for (auto immutablePath = immutablePaths.begin(); immutablePath != immutablePaths.end();
             ++immutablePath) {
            if (pathTaken->isPrefixOf(**immutablePath)) {
                original = element.getDocument().getObject();
                break;
            }
        }

        // We found an existing element at the update path.
        updateExistingElement(&element, noop);
        if (*noop) {
            return;  // Successful no-op update.
        }

        if (validateForStorage) {
            const bool doRecursiveCheck = true;
            const uint32_t recursionLevel = pathTaken->numParts();
            storage_validation::storageValid(element, doRecursiveCheck, recursionLevel);
        }

        checkImmutablePathsNotModified(element, pathTaken, immutablePaths, original);

        valueToLog = element;
    } else {
        // We did not find an element at the update path. Create one.
        auto newElementFieldName = pathToCreate->getPart(pathToCreate->numParts() - 1);
        auto newElement = element.getDocument().makeElementNull(newElementFieldName);
        setValueForNewElement(&newElement);

        invariant(newElement.ok());
        auto statusWithFirstCreatedElem =
            pathsupport::createPathAt(*pathToCreate, 0, element, newElement);
        if (!statusWithFirstCreatedElem.isOK()) {
            // $sets on non-viable paths are ignored when the update came from replication. We do
            // not error because idempotency requires that any other update modifiers must still be
            // applied. For example, consider applying the following updates twice to an initially
            // empty document:
            // {$set: {c: 0}}
            // {$set: {'a.b': 0, c: 1}}
            // {$set: {a: 0}}
            // Setting 'a.b' will fail the second time, but we must still set 'c'.
            // (There are modifiers besides $set that use this code path, but they are not used for
            // replication, so we are not concerned with their behavior when "fromReplication" is
            // true.)
            if (statusWithFirstCreatedElem.getStatus().code() == ErrorCodes::PathNotViable &&
                fromReplication) {
                *noop = true;
                return;
            }
            uassertStatusOK(statusWithFirstCreatedElem);
            MONGO_UNREACHABLE;  // The previous uassertStatusOK should always throw.
        }

        if (validateForStorage) {
            const bool doRecursiveCheck = true;
            const uint32_t recursionLevel = pathTaken->numParts() + 1;
            storage_validation::storageValid(
                statusWithFirstCreatedElem.getValue(), doRecursiveCheck, recursionLevel);
        }

        for (auto immutablePath = immutablePaths.begin(); immutablePath != immutablePaths.end();
             ++immutablePath) {

            // If 'immutablePath' is a (strict or non-strict) prefix of 'pathTaken', then we are
            // modifying 'immutablePath'. For example, adding '_id.x' will illegally modify '_id'.
            uassert(ErrorCodes::ImmutableField,
                    str::stream() << "Updating the path '" << pathTaken->dottedField() << "' to "
                                  << element.toString()
                                  << " would modify the immutable field '"
                                  << (*immutablePath)->dottedField()
                                  << "'",
                    pathTaken->commonPrefixSize(**immutablePath) != (*immutablePath)->numParts());
        }

        valueToLog = newElement;
    }

    // Create full field path of set element.
    StringBuilder builder;
    builder << pathTaken->dottedField();
    if (!pathTaken->empty() && !pathToCreate->empty()) {
        builder << ".";
    }
    builder << pathToCreate->dottedField();
    auto fullPath = builder.str();

    // Determine if indexes are affected.
    if (indexData && indexData->mightBeIndexed(fullPath)) {
        *indexesAffected = true;
    }

    // Log the operation.
    if (logBuilder) {
        auto logElement =
            logBuilder->getDocument().makeElementWithNewFieldName(fullPath, valueToLog);
        invariant(logElement.ok());
        uassertStatusOK(logBuilder->addToSets(logElement));
    }
}
}  // namespace mongo
