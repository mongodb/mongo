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

#include "mongo/db/exec/projection_executor.h"

#include "mongo/db/query/projection_policies.h"

namespace mongo::projection_executor {
/**
 * A node used to define the parsed structure of a projection. Each ProjectionNode represents one
 * 'level' of the parsed specification. The root ProjectionNode represents all top level projections
 * or additions, with any child ProjectionNodes representing dotted or nested projections or
 * additions.
 *
 * ProjectionNode is an abstract base class which implements all the generic construction, traversal
 * and execution functionality common to different projection types. Each derived class need only
 * provide a minimal set of virtual function implementations dictating, for instance, how the
 * projection should behave upon reaching a leaf node.
 */
class ProjectionNode {
public:
    ProjectionNode(ProjectionPolicies policies, std::string pathToNode = "");

    virtual ~ProjectionNode() = default;

    /**
     * Recursively adds 'path' into the tree as a projected field, creating any child nodes if
     * necessary.
     *
     * 'path' is allowed to be dotted, and is assumed not to conflict with another path already in
     * the tree. For example, it is an error to add the path "a.b" from a tree which has already
     * added a computed field "a".
     *
     * Note: This function can only be called from the root of the tree. This will ensure that all
     * the node properties are set correctly along the path from root to expression leaf.
     */
    void addProjectionForPath(const FieldPath& path);

    /**
     * Get the expression for the given path. Returns null if no expression for the given path is
     * found.
     */
    boost::intrusive_ptr<Expression> getExpressionForPath(const FieldPath& path) const;

    /**
     * Recursively adds 'path' into the tree as a computed field, creating any child nodes if
     * necessary.
     *
     * 'path' is allowed to be dotted, and is assumed not to conflict with another path already in
     * the tree. For example, it is an error to add the path "a.b" as a computed field to a tree
     * which has already projected the field "a".
     *
     * Note: This function can only be called from the root of the tree. Every node needs to know
     * whether it has an expression as a descendent, so by adding nodes only from the root, this
     * flag can be set correctly along the path from root to expression leaf.
     */
    void addExpressionForPath(const FieldPath& path, boost::intrusive_ptr<Expression> expr);

    /**
     * Applies all projections and expressions, if applicable, and returns the resulting document.
     */
    virtual Document applyToDocument(const Document& inputDoc) const;

    /**
     * Recursively evaluates all expressions in the projection, writing the results to 'outputDoc'.
     */
    void applyExpressions(const Document& root, MutableDocument* outputDoc) const;

    /**
     * Reports dependencies on any fields that are required by this projection.
     */
    virtual void reportDependencies(DepsTracker* deps) const = 0;

    /**
     * Recursively report all paths that are referenced by this projection.
     */
    void reportProjectedPaths(OrderedPathSet* preservedPaths) const;

    /**
     * Return an optional number, x, which indicates that it is safe to stop reading the document
     * being projected once x fields have been projected.
     */
    virtual boost::optional<size_t> maxFieldsToProject() const {
        return boost::none;
    }

    /**
     * Recursively reports all computed paths in this projection, adding them into 'computedPaths'.
     *
     * Computed paths that are identified as the result of a simple rename are instead filled out in
     * 'renamedPaths'. Each entry in 'renamedPaths' maps from the path's new name to its old name
     * prior to application of this projection.
     */
    void reportComputedPaths(OrderedPathSet* computedPaths,
                             StringMap<std::string>* renamedPaths) const;

    const std::string& getPath() const {
        return _pathToNode;
    }

    void optimize();

    Document serialize(boost::optional<ExplainOptions::Verbosity> explain) const;

    void serialize(boost::optional<ExplainOptions::Verbosity> explain,
                   MutableDocument* output) const;

protected:
    /**
     * Creates the child if it doesn't already exist. 'field' is not allowed to be dotted. Returns
     * the child node if it already exists, or the newly-created child otherwise.
     */
    ProjectionNode* addOrGetChild(const std::string& field);

    // Returns a unique_ptr to a new instance of the implementing class for the given 'fieldName'.
    virtual std::unique_ptr<ProjectionNode> makeChild(const std::string& fieldName) const = 0;

    // Returns the initial document to which the current level of the projection should be applied.
    // For an inclusion projection this will be an empty document, to which we will add the fields
    // we wish to retain. For an exclusion this will be the complete document, from which we will
    // eliminate the fields we wish to omit.
    virtual MutableDocument initializeOutputDocument(const Document& inputDoc) const = 0;

    // Given an input leaf value, returns the value that should be added to the output document.
    // Depending on the projection type this will be either the value itself, or "missing".
    virtual Value applyLeafProjectionToValue(const Value& value) const = 0;

    // Given an input leaf that we intend to skip over, returns the value that should be added to
    // the output document. Depending on the projection this will be either the value, or "missing".
    virtual Value transformSkippedValueForOutput(const Value&) const = 0;

    // Writes the given value to the output doc, replacing the existing value of 'field' if present.
    virtual void outputProjectedField(StringData field, Value val, MutableDocument* outDoc) const;

    StringMap<std::unique_ptr<ProjectionNode>> _children;
    StringMap<boost::intrusive_ptr<Expression>> _expressions;
    StringSet _projectedFields;
    ProjectionPolicies _policies;
    std::string _pathToNode;

    // Whether this node or any child of this node contains a computed field.
    bool _subtreeContainsComputedFields{false};

    // Our projection semantics are such that all field additions need to be processed in the order
    // specified. '_orderToProcessAdditionsAndChildren' tracks that order.
    //
    // For example, for the specification {a: <expression>, "b.c": <expression>, d: <expression>},
    // we need to add the top level fields in the order "a", then "b", then "d". This ordering
    // information needs to be tracked separately, since "a" and "d" will be tracked via
    // '_expressions', and "b.c" will be tracked as a child ProjectionNode in '_children'. For the
    // example above, '_orderToProcessAdditionsAndChildren' would be ["a", "b", "d"].
    std::vector<std::string> _orderToProcessAdditionsAndChildren;

private:
    // Iterates 'inputDoc' for each projected field, adding to or removing from 'outputDoc'. Also
    // copies over enough information to preserve the structure of the incoming document for the
    // fields this projection cares about.
    //
    // For example, given a ProjectionNode tree representing this projection:
    //    {a: {b: 1, c: <exp>}, "d.e": <exp>}
    // Calling applyProjections() with an 'inputDoc' of
    //    {a: [{b: 1, d: 1}, {b: 2, d: 2}], d: [{e: 1, f: 1}, {e: 1, f: 1}]}
    // and an empty 'outputDoc' will leave 'outputDoc' representing the document
    //    {a: [{b: 1}, {b: 2}], d: [{}, {}]}
    void applyProjections(const Document& inputDoc, MutableDocument* outputDoc) const;

    // Helpers for the 'applyProjections' and 'applyExpressions' methods. Applies the transformation
    // recursively to each element of any arrays, and ensures primitives are handled appropriately.
    Value applyExpressionsToValue(const Document& root, Value inputVal) const;
    Value applyProjectionsToValue(Value inputVal) const;

    // Adds a new ProjectionNode as a child. 'field' cannot be dotted.
    ProjectionNode* addChild(const std::string& field);

    // Returns nullptr if no such child exists.
    ProjectionNode* getChild(const std::string& field) const;

    /**
     * Indicates that metadata computed by previous calls to optimize() is now stale and must be
     * recomputed. This must be called any time the tree is updated (an expression added or child
     * node added).
     */
    void makeOptimizationsStale() {
        _maxFieldsToProject = boost::none;
    }

    /**
     * Internal helper function for addExpressionForPath().
     */
    void _addExpressionForPath(const FieldPath& path, boost::intrusive_ptr<Expression> expr);

    /**
     * Internal helper function for addProjectionForPath().
     */
    void _addProjectionForPath(const FieldPath& path);

    // Maximum number of fields that need to be projected. This allows for an "early" return
    // optimization which means we don't have to iterate over an entire document. The value is
    // stored here to avoid re-computation for each document.
    boost::optional<size_t> _maxFieldsToProject;
};
}  // namespace mongo::projection_executor
