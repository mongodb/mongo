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

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/transformer_interface.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/compiler/dependency_analysis/expression_dependencies.h"
#include "mongo/db/query/query_shape/serialization_options.h"

#include <set>
#include <string>
#include <utility>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

/**
 * This class implements the transformation logic for the $replaceRoot and $replaceWith stages.
 */
class ReplaceRootTransformation final : public TransformerInterface {
public:
    enum class UserSpecifiedName { kReplaceRoot, kReplaceWith };

    ReplaceRootTransformation(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                              boost::intrusive_ptr<Expression> newRootExpression,
                              std::string errMsgContextForNonObject,
                              SbeCompatibility sbeCompatibility)
        : _expCtx(expCtx),
          _newRoot(std::move(newRootExpression)),
          _errMsgContextForNonObject(std::move(errMsgContextForNonObject)) {
        _sbeCompatibility = sbeCompatibility;
    }

    TransformerType getType() const final {
        return TransformerType::kReplaceRoot;
    }

    Document applyTransformation(const Document& input) const final;

    // Optimize the newRoot expression.
    void optimize() final {
        // Optimization can sometimes modify a previously compatible expression so that it can no
        // longer be executed in SBE. When that happens, the expression optimizer updates the
        // 'sbeCompatibility' value in the ExpressionContext, which we can use to update the
        // '_sbeCompatibility' value for this $replaceRoot operation.
        SbeCompatibility originalSbeCompatibility =
            _expCtx->sbeCompatibilityExchange(_sbeCompatibility);
        ON_BLOCK_EXIT([&] { this->_expCtx->setSbeCompatibility(originalSbeCompatibility); });

        _newRoot = _newRoot->optimize();

        _sbeCompatibility = _expCtx->getSbeCompatibility();
    }

    // Used by optimize() to optimize out the $replaceRoot stage if it is a no-op.
    // In particular, the stage {$replaceRoot: {newRoot: '$$ROOT'}} would get optimized out.
    // Since $$ROOT is a reference to the top-level document currently being processed in
    // the pipeline, the root would remain the same.
    bool isNoop() const final {
        auto fieldPath = dynamic_cast<ExpressionFieldPath*>(_newRoot.get());
        return fieldPath && fieldPath->isROOT();
    }

    Document serializeTransformation(const SerializationOptions& options = {}) const final {
        return Document{{"newRoot", _newRoot->serialize(options)}};
    }

    DepsTracker::State addDependencies(DepsTracker* deps) const final {
        expression::addDependencies(_newRoot.get(), deps);
        // This stage will replace the entire document with a new document, so any existing fields
        // will be replaced and cannot be required as dependencies.
        return DepsTracker::State::EXHAUSTIVE_FIELDS;
    }

    DocumentSource::GetModPathsReturn getModifiedPaths() const final {
        // Replaces the entire root, so all paths are modified.
        return {DocumentSource::GetModPathsReturn::Type::kAllPaths, OrderedPathSet{}, {}};
    }

    const boost::intrusive_ptr<Expression>& getExpression() const {
        return _newRoot;
    }

    /**
     * Detects if 'replaceRootTransform' represents the unnesting of a field path. If it does,
     * returns the name of that field path. For example, if 'replaceRootTransform' represents the
     * transformation associated with {$replaceWith: "$x"} or {$replaceRoot: {newRoot: "$x"}},
     * returns "x".
     */
    boost::optional<std::string> unnestsPath() const;

    /**
     * This whole block adds
     * {$or: [{"subDocument": {$type: "array"}}, {"subDocument": {$not: {$type: "object"}}}]}
     * in order to match on documents which, when evaluated at $expression, don't resolve to
     * objects. "array" is separately included because for MatchExpressions, arrays are evaluated as
     * "object" types.
     */
    static boost::intrusive_ptr<DocumentSourceMatch> createTypeNEObjectPredicate(
        const std::string& expression, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    /**
     * Report renames from this stage to a MatchExpression. We only report a rename for a top-level
     * field in a path to ensure that we only have at max one applicable rename per path in the ME.
     */
    void reportRenames(const MatchExpression* expr,
                       const std::string& prefixPath,
                       StringMap<std::string>& renames);

    /**
     * This optimization pushes a match stage before a replaceRoot stage to improve performance.
     *
     * Ex: [{$replaceWith: {"$subDocument"}}, {$match: {x: 2}}]
     * ->
     * [{$match: {"subDocument.x": 2}}, {$replaceWith: {"$subDocument"}}]
     *
     * We also append {$expr: {$ne: [{$type: "$expression"}, {$const: "object"}]}} to the pushed
     * forward match stage to ensure that after we swap a $match stage before a $replaceRoot stage,
     * we can still check that the structure of all documents in the collection can resolve to a
     * document after `newRoot` is applied.
     *
     * Note: this optimization will not rename and push forward a match stage if there are
     * dependencies between different paths contained in the match expression.
     */
    bool pushDotRenamedMatchBefore(DocumentSourceContainer::iterator itr,
                                   DocumentSourceContainer* container);

    DocumentSourceContainer::iterator doOptimizeAt(DocumentSourceContainer::iterator itr,
                                                   DocumentSourceContainer* container) final;

    boost::intrusive_ptr<Expression>& getExpressionToModify() {
        return _newRoot;
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const final {
        expression::addVariableRefs(_newRoot.get(), refs);
    }

    SbeCompatibility sbeCompatibility() const {
        return _sbeCompatibility;
    }

private:
    const boost::intrusive_ptr<ExpressionContext> _expCtx;
    boost::intrusive_ptr<Expression> _newRoot;

    // A string for additional context for the user about where/why we were expecting an object.
    // This can be helpful if you are using $replaceRoot as part of an alias expansion as we do in
    // $documents for example. Goes first in the template error message below.
    std::string _errMsgContextForNonObject;
    static constexpr StringData kErrorTemplate =
        "{} must evaluate to an object, but resulting value was: {}. Type of resulting value: "
        "'{}'. Input document: {}"_sd;

    SbeCompatibility _sbeCompatibility = SbeCompatibility::notCompatible;
};

/*
 * $replaceRoot takes an object containing only an expression in the newRoot field, and replaces
 * each incoming document with the result of evaluating that expression. Throws an error if the
 * given expression is not an object or if the expression evaluates to the "missing" Value. This
 * is implemented as an extension of DocumentSourceSingleDocumentTransformation.
 *
 * There is a shorthand $replaceWith alias which takes a direct single argument containing the
 * expression which will become the new root: {$replaceWith: <expression>} aliases to {$replaceRoot:
 * {newRoot: <expression>}}.
 */
class DocumentSourceReplaceRoot final {
public:
    static constexpr StringData kStageName = "$replaceRoot"_sd;
    static constexpr StringData kAliasNameReplaceWith = "$replaceWith"_sd;
    /**
     * Creates a new replaceRoot DocumentSource from the BSON specification of the $replaceRoot
     * stage.
     */
    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    static boost::intrusive_ptr<DocumentSource> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const boost::intrusive_ptr<Expression>& newRootExpression,
        std::string errMsgContextForNonObjects,
        SbeCompatibility sbeCompatibility);

private:
    // It is illegal to construct a DocumentSourceReplaceRoot directly, use createFromBson()
    // instead.
    DocumentSourceReplaceRoot() = default;
};

}  // namespace mongo
