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

#include "mongo/db/update/array_culling_node.h"

namespace mongo {

UpdateNode::ApplyResult ArrayCullingNode::apply(ApplyParams applyParams) const {
    if (!applyParams.pathToCreate->empty()) {
        // There were path components we could not traverse. We treat this as a no-op, unless it
        // would have been impossible to create those elements, which we check with
        // checkViability().
        UpdateLeafNode::checkViability(
            applyParams.element, *(applyParams.pathToCreate), *(applyParams.pathTaken));

        return ApplyResult::noopResult();
    }

    // This operation only applies to arrays
    uassert(ErrorCodes::BadValue,
            "Cannot apply $pull to a non-array value",
            applyParams.element.getType() == mongo::Array);

    size_t numRemoved = 0;
    auto cursor = applyParams.element.leftChild();
    while (cursor.ok()) {
        // Make sure to get the next array element now, because if we remove the 'cursor' element,
        // the rightSibling pointer will be invalidated.
        auto nextElement = cursor.rightSibling();
        if (_matcher->match(cursor)) {
            invariantOK(cursor.remove());
            numRemoved++;
        }
        cursor = nextElement;
    }

    if (numRemoved == 0) {
        return ApplyResult::noopResult();  // Skip the index check, immutable path check, and
                                           // logging steps.
    }

    ApplyResult applyResult;

    // Determine if indexes are affected.
    if (!applyParams.indexData ||
        !applyParams.indexData->mightBeIndexed(applyParams.pathTaken->dottedField())) {
        applyResult.indexesAffected = false;
    }

    // No need to validate for storage, since we cannot have increased the BSON depth or interfered
    // with a DBRef.

    // Ensure we are not changing any immutable paths.
    for (const auto& immutablePath : applyParams.immutablePaths) {
        uassert(ErrorCodes::ImmutableField,
                str::stream() << "Performing an update on the path '"
                              << applyParams.pathTaken->dottedField()
                              << "' would modify the immutable field '"
                              << immutablePath->dottedField()
                              << "'",
                applyParams.pathTaken->commonPrefixSize(*immutablePath) <
                    std::min(applyParams.pathTaken->numParts(), immutablePath->numParts()));
    }

    if (applyParams.logBuilder) {
        auto& doc = applyParams.logBuilder->getDocument();
        auto logElement = doc.makeElementArray(applyParams.pathTaken->dottedField());

        for (auto cursor = applyParams.element.leftChild(); cursor.ok();
             cursor = cursor.rightSibling()) {
            dassert(cursor.hasValue());

            auto copy = doc.makeElementWithNewFieldName(StringData(), cursor.getValue());
            uassert(ErrorCodes::InternalError, "could not create copy element", copy.ok());
            uassertStatusOK(logElement.pushBack(copy));
        }

        uassertStatusOK(applyParams.logBuilder->addToSets(logElement));
    }

    return applyResult;
}

}  // namespace mongo
