/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/pipeline/group_from_first_document_transformation.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/query/compiler/dependency_analysis/expression_dependencies.h"

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
Document GroupFromFirstDocumentTransformation::applyTransformation(const Document& input) const {
    MutableDocument output(_accumulatorExprs.size());

    for (auto&& expr : _accumulatorExprs) {
        auto value = expr.second->evaluate(input, &expr.second->getExpressionContext()->variables);
        output.addField(expr.first, value.missing() ? Value(BSONNULL) : std::move(value));
    }

    return output.freeze();
}

void GroupFromFirstDocumentTransformation::optimize() {
    for (auto&& expr : _accumulatorExprs) {
        expr.second = expr.second->optimize();
    }
}

DocumentSourceContainer::iterator GroupFromFirstDocumentTransformation::doOptimizeAt(
    DocumentSourceContainer::iterator itr, DocumentSourceContainer* container) {
    return std::next(itr);
}

Document GroupFromFirstDocumentTransformation::serializeTransformation(
    const SerializationOptions& options) const {
    MutableDocument newRoot(_accumulatorExprs.size());

    for (auto&& expr : _accumulatorExprs) {
        newRoot.addField(expr.first, expr.second->serialize(options));
    }

    return {{"newRoot", newRoot.freezeToValue()}};
}

DepsTracker::State GroupFromFirstDocumentTransformation::addDependencies(DepsTracker* deps) const {
    for (auto&& expr : _accumulatorExprs) {
        expression::addDependencies(expr.second.get(), deps);
    }

    // This stage will replace the entire document with a new document, so any existing fields
    // will be replaced and cannot be required as dependencies. We use EXHAUSTIVE_ALL here
    // instead of EXHAUSTIVE_FIELDS, as in ReplaceRootTransformation, because the stages that
    // follow a $group stage should not depend on document metadata.
    return DepsTracker::State::EXHAUSTIVE_ALL;
}

void GroupFromFirstDocumentTransformation::addVariableRefs(std::set<Variables::Id>* refs) const {
    for (auto&& expr : _accumulatorExprs) {
        expression::addVariableRefs(expr.second.get(), refs);
    }
}

DocumentSource::GetModPathsReturn GroupFromFirstDocumentTransformation::getModifiedPaths() const {
    // Replaces the entire root, so all paths are modified.
    return {DocumentSource::GetModPathsReturn::Type::kAllPaths, OrderedPathSet{}, {}};
}

std::unique_ptr<GroupFromFirstDocumentTransformation> GroupFromFirstDocumentTransformation::create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const std::string& groupId,
    StringData originalStageName,
    std::vector<std::pair<std::string, boost::intrusive_ptr<Expression>>> accumulatorExprs,
    AccumulatorDocumentsNeeded docsNeeded) {
    return std::make_unique<GroupFromFirstDocumentTransformation>(
        groupId, originalStageName, std::move(accumulatorExprs), docsNeeded);
}

}  // namespace mongo
