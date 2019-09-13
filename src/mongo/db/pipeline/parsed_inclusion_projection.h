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
#include "mongo/db/pipeline/parsed_aggregation_projection_node.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/stdx/unordered_set.h"

namespace mongo {

class FieldPath;
class Value;

namespace parsed_aggregation_projection {

/**
 * A node used to define the parsed structure of an inclusion projection. Each InclusionNode
 * represents one 'level' of the parsed specification. The root InclusionNode represents all top
 * level inclusions or additions, with any child InclusionNodes representing dotted or nested
 * inclusions or additions.
 */
class InclusionNode final : public ProjectionNode {
public:
    InclusionNode(ProjectionPolicies policies, std::string pathToNode = "");

    InclusionNode* addOrGetChild(const std::string& field);

    void reportDependencies(DepsTracker* deps) const final;

protected:
    // For inclusions, we can apply an optimization here by simply appending to the output document
    // via MutableDocument::addField, rather than always checking for existing fields via setField.
    void outputProjectedField(StringData field, Value val, MutableDocument* outputDoc) const final {
        outputDoc->addField(field, val);
    }
    std::unique_ptr<ProjectionNode> makeChild(std::string fieldName) const final {
        return std::make_unique<InclusionNode>(
            _policies, FieldPath::getFullyQualifiedPath(_pathToNode, fieldName));
    }
    Document initializeOutputDocument(const Document& inputDoc) const final {
        return {};
    }
    Value applyLeafProjectionToValue(const Value& value) const final {
        return value;
    }
    Value transformSkippedValueForOutput(const Value& value) const final {
        return Value();
    }
};

/**
 * A ParsedInclusionProjection represents a parsed form of the raw BSON specification.
 *
 * This class is mostly a wrapper around an InclusionNode tree. It contains logic to parse a
 * specification object into the corresponding InclusionNode tree, but defers most execution logic
 * to the underlying tree.
 */
class ParsedInclusionProjection : public ParsedAggregationProjection {
public:
    ParsedInclusionProjection(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                              ProjectionPolicies policies,
                              std::unique_ptr<InclusionNode> root)
        : ParsedAggregationProjection(expCtx, policies), _root(std::move(root)) {}

    ParsedInclusionProjection(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                              ProjectionPolicies policies)
        : ParsedInclusionProjection(expCtx, policies, std::make_unique<InclusionNode>(policies)) {}

    TransformerType getType() const final {
        return TransformerType::kInclusionProjection;
    }

    const InclusionNode& getRoot() const {
        return *_root;
    }

    /**
     * Parses the projection specification given by 'spec', populating internal data structures.
     */
    void parse(const BSONObj& spec) final;

    /**
     * Serialize the projection.
     */
    Document serializeTransformation(
        boost::optional<ExplainOptions::Verbosity> explain) const final {
        MutableDocument output;
        if (_idExcluded) {
            output.addField("_id", Value(false));
        }
        _root->serialize(explain, &output);
        return output.freeze();
    }

    /**
     * Optimize any computed expressions.
     */
    void optimize() final {
        ParsedAggregationProjection::optimize();
        _root->optimize();
    }

    DepsTracker::State addDependencies(DepsTracker* deps) const final {
        _root->reportDependencies(deps);
        if (_rootReplacementExpression) {
            _rootReplacementExpression->addDependencies(deps);
        }
        return DepsTracker::State::EXHAUSTIVE_FIELDS;
    }

    DocumentSource::GetModPathsReturn getModifiedPaths() const final {
        // A root-replacement expression can replace the entire root document, so all paths are
        // considered as modified.
        if (_rootReplacementExpression) {
            return {DocumentSource::GetModPathsReturn::Type::kAllPaths, {}, {}};
        }

        std::set<std::string> preservedPaths;
        _root->reportProjectedPaths(&preservedPaths);

        std::set<std::string> computedPaths;
        StringMap<std::string> renamedPaths;
        _root->reportComputedPaths(&computedPaths, &renamedPaths);

        return {DocumentSource::GetModPathsReturn::Type::kAllExcept,
                std::move(preservedPaths),
                std::move(renamedPaths)};
    }

    /**
     * Apply this exclusion projection to 'inputDoc'.
     *
     * All inclusions are processed before all computed fields. Computed fields will be added
     * afterwards in the order in which they were specified to the $project stage.
     *
     * Arrays will be traversed, with any dotted/nested exclusions or computed fields applied to
     * each element in the array.
     */
    Document applyProjection(const Document& inputDoc) const final;

    /*
     * Checks whether the inclusion projection represented by the InclusionNode
     * tree is a subset of the object passed in. Projections that have any
     * computed or renamed fields are not considered a subset.
     */
    bool isSubsetOfProjection(const BSONObj& proj) const final;

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
     * Traverses 'subObj' and parses each field. Adds any included or computed fields at this level
     * to 'node'.
     */
    void parseSubObject(const BSONObj& subObj,
                        const VariablesParseState& variablesParseState,
                        InclusionNode* node);

    // Not strictly necessary to track here, but makes serialization easier.
    bool _idExcluded = false;

    // The InclusionNode tree does most of the execution work once constructed.
    std::unique_ptr<InclusionNode> _root;
};
}  // namespace parsed_aggregation_projection
}  // namespace mongo
