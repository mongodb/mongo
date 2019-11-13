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

#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/parsed_aggregation_projection.h"
#include "mongo/db/pipeline/parsed_inclusion_projection.h"

namespace mongo {
namespace parsed_aggregation_projection {
/**
 * This class ensures that the specification was valid: that none of the paths specified conflict
 * with one another, that there is at least one field, etc. Here "projection" includes $addFields
 * specifications.
 */
class ProjectionSpecValidator {
public:
    /**
     * Throws if the specification is not valid for a projection. Because this validator is meant to
     * be generic, the error thrown is generic.  Callers at the DocumentSource level should modify
     * the error message if they want to include information specific to the stage name used.
     */
    static void uassertValid(const BSONObj& spec);

private:
    ProjectionSpecValidator(const BSONObj& spec) : _rawObj(spec) {}

    /**
     * Uses '_seenPaths' to see if 'path' conflicts with any paths that have already been specified.
     *
     * For example, a user is not allowed to specify {'a': 1, 'a.b': 1}, or some similar conflicting
     * paths.
     */
    void ensurePathDoesNotConflictOrThrow(const std::string& path);

    /**
     * Throws if an invalid projection specification is detected.
     */
    void validate();

    /**
     * Parses a single BSONElement. 'pathToElem' should include the field name of 'elem'.
     *
     * Delegates to parseSubObject() if 'elem' is an object. Otherwise adds the full path to 'elem'
     * to '_seenPaths'.
     *
     * Calls ensurePathDoesNotConflictOrThrow with the path to this element, throws on conflicting
     * path specifications.
     */
    void parseElement(const BSONElement& elem, const FieldPath& pathToElem);

    /**
     * Traverses 'thisLevelSpec', parsing each element in turn.
     *
     * Throws if any paths conflict with each other or existing paths, 'thisLevelSpec' contains a
     * dotted path, or if 'thisLevelSpec' represents an invalid expression.
     */
    void parseNestedObject(const BSONObj& thisLevelSpec, const FieldPath& prefix);

    // The original object. Used to generate more helpful error messages.
    const BSONObj& _rawObj;

    // Custom comparator that orders fieldpath strings by path prefix first, then by field.
    struct PathPrefixComparator {
        static constexpr char dot = '.';

        // Returns true if the lhs value should sort before the rhs, false otherwise.
        bool operator()(const std::string& lhs, const std::string& rhs) const {
            for (size_t pos = 0, len = std::min(lhs.size(), rhs.size()); pos < len; ++pos) {
                auto &lchar = lhs[pos], &rchar = rhs[pos];
                if (lchar == rchar) {
                    continue;
                }

                // Consider the path delimiter '.' as being less than all other characters, so that
                // paths sort directly before any paths they prefix and directly after any paths
                // which prefix them.
                if (lchar == dot) {
                    return true;
                } else if (rchar == dot) {
                    return false;
                }

                // Otherwise, default to normal character comparison.
                return lchar < rchar;
            }

            // If we get here, then we have reached the end of lhs and/or rhs and all of their path
            // segments up to this point match. If lhs is shorter than rhs, then lhs prefixes rhs
            // and should sort before it.
            return lhs.size() < rhs.size();
        }
    };

    // Tracks which paths we've seen to ensure no two paths conflict with each other.
    std::set<std::string, PathPrefixComparator> _seenPaths;
};

/**
 * A ParsedAddFields represents a parsed form of the raw BSON specification for the AddFields
 * stage.
 *
 * This class is mostly a wrapper around an InclusionNode tree. It contains logic to parse a
 * specification object into the corresponding InclusionNode tree, but defers most execution logic
 * to the underlying tree. In this way it is similar to ParsedInclusionProjection, but it differs
 * by not applying inclusions before adding computed fields, thus keeping all existing fields.
 */
class ParsedAddFields : public ParsedAggregationProjection {
public:
    /**
     * TODO SERVER-25510: The ParsedAggregationProjection _id and array-recursion policies are not
     * applicable to the $addFields "projection" stage. We make them non-configurable here.
     */
    ParsedAddFields(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ParsedAggregationProjection(
              expCtx,
              {ProjectionPolicies::DefaultIdPolicy::kIncludeId,
               ProjectionPolicies::ArrayRecursionPolicy::kRecurseNestedArrays,
               ProjectionPolicies::ComputedFieldsPolicy::kAllowComputedFields}),
          _root(new InclusionNode(_policies)) {}

    /**
     * Creates the data needed to perform an AddFields.
     * Verifies that there are no conflicting paths in the specification.
     * Overrides the ParsedAggregationProjection's create method.
     */
    static std::unique_ptr<ParsedAddFields> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx, const BSONObj& spec);

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
        std::set<std::string> computedPaths;
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

private:
    /**
     * Attempts to parse 'objSpec' as an expression like {$add: [...]}. Adds a computed field to
     * '_root' and returns true if it was successfully parsed as an expression. Returns false if it
     * was not an expression specification.
     *
     * Throws an error if it was determined to be an expression specification, but failed to parse
     * as a valid expression.
     */
    bool parseObjectAsExpression(StringData pathToObject,
                                 const BSONObj& objSpec,
                                 const VariablesParseState& variablesParseState);

    /**
     * Traverses 'subObj' and parses each field. Adds any computed fields at this level
     * to 'node'.
     */
    void parseSubObject(const BSONObj& subObj,
                        const VariablesParseState& variablesParseState,
                        InclusionNode* node);

    // The InclusionNode tree does most of the execution work once constructed.
    std::unique_ptr<InclusionNode> _root;
};
}  // namespace parsed_aggregation_projection
}  // namespace mongo
