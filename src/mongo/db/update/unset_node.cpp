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

#include "mongo/db/update/unset_node.h"

#include "mongo/db/update/storage_validation.h"

namespace mongo {

Status UnsetNode::init(BSONElement modExpr, const CollatorInterface* collator) {
    // Note that we don't need to store modExpr, because $unset does not do anything with its value.
    invariant(modExpr.ok());
    return Status::OK();
}

UpdateNode::ApplyResult UnsetNode::apply(ApplyParams applyParams) const {
    if (!applyParams.pathToCreate->empty()) {
        // A non-empty "pathToCreate" implies that our search did not find the field that we wanted
        // to delete. We employ a simple and efficient strategy for deleting fields that don't yet
        // exist.
        return ApplyResult::noopResult();
    }

    ApplyResult applyResult;

    // Determine if indexes are affected.
    if (!applyParams.indexData ||
        !applyParams.indexData->mightBeIndexed(applyParams.pathTaken->dottedField())) {
        applyResult.indexesAffected = false;
    }

    auto parent = applyParams.element.parent();
    auto leftSibling = applyParams.element.leftSibling();
    auto rightSibling = applyParams.element.rightSibling();

    invariant(parent.ok());
    if (!parent.isType(BSONType::Array)) {
        invariantOK(applyParams.element.remove());
    } else {
        // Special case: An $unset on an array element sets it to null instead of removing it from
        // the array.
        invariantOK(applyParams.element.setValueNull());
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
                str::stream() << "Unsetting the path '" << applyParams.pathTaken->dottedField()
                              << "' would modify the immutable field '"
                              << (*immutablePath)->dottedField()
                              << "'",
                applyParams.pathTaken->commonPrefixSize(**immutablePath) <
                    std::min(applyParams.pathTaken->numParts(), (*immutablePath)->numParts()));
    }

    // Log the unset.
    if (applyParams.logBuilder) {
        uassertStatusOK(applyParams.logBuilder->addToUnsets(applyParams.pathTaken->dottedField()));
    }

    return applyResult;
}

}  // namespace mongo
