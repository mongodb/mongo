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

#include "mongo/db/update/pop_node.h"

#include "mongo/db/matcher/expression_parser.h"

namespace mongo {

Status PopNode::init(BSONElement modExpr, const CollatorInterface* collator) {
    auto popVal = MatchExpressionParser::parseIntegerElementToLong(modExpr);
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

UpdateNode::ApplyResult PopNode::apply(ApplyParams applyParams) const {
    if (applyParams.pathTaken->empty()) {
        // No components of the path existed. The pop is treated as a no-op in this case.
        return ApplyResult::noopResult();
    }

    if (!applyParams.pathToCreate->empty()) {
        // There were path components we could not traverse. We treat this as a no-op, unless it
        // would have been impossible to create those elements, which we check with
        // checkViability().
        UpdateLeafNode::checkViability(
            applyParams.element, *(applyParams.pathToCreate), *(applyParams.pathTaken));

        return ApplyResult::noopResult();
    }

    invariant(!applyParams.pathTaken->empty());
    invariant(applyParams.pathToCreate->empty());

    // The full path existed, but we must fail if the element at that path is not an array.
    invariant(applyParams.element.ok());
    uassert(ErrorCodes::TypeMismatch,
            str::stream() << "Path '" << applyParams.pathTaken->dottedField()
                          << "' contains an element of non-array type '"
                          << typeName(applyParams.element.getType())
                          << "'",
            applyParams.element.getType() == BSONType::Array);

    if (!applyParams.element.hasChildren()) {
        // The path exists and contains an array, but the array is empty.
        return ApplyResult::noopResult();
    }

    ApplyResult applyResult;

    if (!applyParams.indexData ||
        !applyParams.indexData->mightBeIndexed(applyParams.pathTaken->dottedField())) {
        applyResult.indexesAffected = false;
    }

    auto elementToRemove =
        _popFromFront ? applyParams.element.leftChild() : applyParams.element.rightChild();
    invariantOK(elementToRemove.remove());

    // No need to validate for storage, since we cannot have increased the BSON depth or interfered
    // with a DBRef.

    // Ensure we are not changing any immutable paths.
    for (auto immutablePath = applyParams.immutablePaths.begin();
         immutablePath != applyParams.immutablePaths.end();
         ++immutablePath) {
        uassert(ErrorCodes::ImmutableField,
                str::stream() << "Performing a $pop on the path '"
                              << applyParams.pathTaken->dottedField()
                              << "' would modify the immutable field '"
                              << (*immutablePath)->dottedField()
                              << "'",
                applyParams.pathTaken->commonPrefixSize(**immutablePath) <
                    std::min(applyParams.pathTaken->numParts(), (*immutablePath)->numParts()));
    }

    if (applyParams.logBuilder) {
        uassertStatusOK(applyParams.logBuilder->addToSetsWithNewFieldName(
            applyParams.pathTaken->dottedField(), applyParams.element));
    }

    return applyResult;
}

}  // namespace mongo
