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

#include <memory>

#include "mongo/db/exec/inclusion_projection_executor.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"

namespace mongo::projection_executor {
/**
 * A AddFieldsProjectionExecutor represents a projection execution tree for the $addFields stage.
 *
 * This class is mostly a wrapper around an InclusionNode tree. It contains logic to parse a
 * specification object into the corresponding InclusionNode tree, but defers most execution logic
 * to the underlying tree. In this way it is similar to InclusionProjectionExecutor, but it differs
 * by not applying inclusions before adding computed fields, thus keeping all existing fields.
 */
class AddFieldsProjectionExecutor : public ProjectionExecutor {
public:
    /**
     * TODO SERVER-25510: The ProjectionExecutor _id and array-recursion policies are not
     * applicable to the $addFields "projection" stage. We make them non-configurable here.
     */
    AddFieldsProjectionExecutor(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ProjectionExecutor(expCtx,
                             {ProjectionPolicies::DefaultIdPolicy::kIncludeId,
                              ProjectionPolicies::ArrayRecursionPolicy::kRecurseNestedArrays,
                              ProjectionPolicies::ComputedFieldsPolicy::kAllowComputedFields}),
          _root(new InclusionNode(_policies)) {}

    AddFieldsProjectionExecutor(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                std::unique_ptr<InclusionNode> root)
        : ProjectionExecutor(expCtx,
                             {ProjectionPolicies::DefaultIdPolicy::kIncludeId,
                              ProjectionPolicies::ArrayRecursionPolicy::kRecurseNestedArrays,
                              ProjectionPolicies::ComputedFieldsPolicy::kAllowComputedFields}),
          _root(std::move(root)) {}
    /**
     * Creates the data needed to perform an AddFields.
     * Verifies that there are no conflicting paths in the specification.
     */
    static std::unique_ptr<AddFieldsProjectionExecutor> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx, const BSONObj& spec);

    /**
     * Create a projection that binds an expression to a top-level field.
     *
     * 'fieldPath' must be a top-level field name (exactly one element; no dots).
     */
    static std::unique_ptr<AddFieldsProjectionExecutor> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const FieldPath& fieldPath,
        const boost::intrusive_ptr<Expression>& expr);

    TransformerType getType() const final {
        return TransformerType::kComputedProjection;
    }

    const InclusionNode& getRoot() const {
        return *_root;
    }

    /**
     * Parses the addFields specification given by 'spec', populating internal data structures.
     */
    void parse(const BSONObj& spec);

    Document serializeTransformation(
        boost::optional<ExplainOptions::Verbosity> explain) const final {
        return _root->serialize(explain);
    }

    /**
     * Optimizes any computed expressions.
     */
    void optimize() final {
        _root->optimize();
    }

    DepsTracker::State addDependencies(DepsTracker* deps) const final {
        _root->reportDependencies(deps);
        return DepsTracker::State::SEE_NEXT;
    }

    DocumentSource::GetModPathsReturn getModifiedPaths() const final {
        OrderedPathSet computedPaths;
        StringMap<std::string> renamedPaths;
        _root->reportComputedPaths(&computedPaths, &renamedPaths);
        return {DocumentSource::GetModPathsReturn::Type::kFiniteSet,
                std::move(computedPaths),
                std::move(renamedPaths)};
    }

    /**
     * Add the specified fields to 'inputDoc'.
     *
     * Replaced fields will remain in their original place in the document, while new added fields
     * will be added to the end of the document in the order in which they were specified to the
     * $addFields stage.
     *
     * Arrays will be traversed, with any dotted/nested computed fields applied to each element in
     * the array. For example, setting "a.0": "hello" will add a field "0" to every object
     * in the array "a". If there is an element in "a" that is not an object, it will be replaced
     * with {"0": "hello"}. See SERVER-25200 for more details.
     */
    Document applyProjection(const Document& inputDoc) const final;

    boost::optional<std::set<FieldRef>> extractExhaustivePaths() const {
        return boost::none;
    }

    std::pair<BSONObj, bool> extractComputedProjections(
        const StringData& oldName,
        const StringData& newName,
        const std::set<StringData>& reservedNames) final {
        return _root->extractComputedProjectionsInAddFields(oldName, newName, reservedNames);
    }

private:
    /**
     * Attempts to parse 'objSpec' as an expression like {$add: [...]}. Adds a computed field to
     * '_root' and returns true if it was successfully parsed as an expression. Returns false if it
     * was not an expression specification.
     *
     * Throws an error if it was determined to be an expression specification, but failed to parse
     * as a valid expression.
     */
    bool parseObjectAsExpression(const FieldPath& pathToObject,
                                 const BSONObj& objSpec,
                                 const VariablesParseState& variablesParseState);

    /**
     * Traverses 'subObj' and parses each field. Creates nodes along the 'pathToObj' when an
     * expression/terminal path is found.
     */
    void parseSubObject(const BSONObj& subObj,
                        const VariablesParseState& variablesParseState,
                        const FieldPath& pathToObj);

    // The InclusionNode tree does most of the execution work once constructed.
    std::unique_ptr<InclusionNode> _root;
};
}  // namespace mongo::projection_executor
