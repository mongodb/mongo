/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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


#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <string>
#include <string_view>

#include <boost/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::pipeline::dependency_graph {

/**
 * A dot-separated field path. Every component is interpreted as a field name and never as an
 * array index.
 */
using PathRef = std::string_view;

/**
 * Callback used to query whether a path from the input of the pipeline (i.e. the base collection)
 * may resolve to an array.
 */
using CanPathBeArray = std::function<bool(std::string_view)>;

/**
 * Always returns true (any path may be an array).
 */
bool defaultCanPathBeArray(std::string_view path);

/**
 * Result of looking up which stage last declared or modified a field path.
 */
struct DeclaringStageResult {
    // The sequence of stages that lead to the stage that last declared or modified the field.
    // nullptr if the field originates from the pipeline input (i.e. the base collection or
    // sub-pipeline input).
    std::vector<boost::intrusive_ptr<mongo::DocumentSource>> srcStages;

    // True when the declaring stage was resolved inside a sub-pipeline (e.g. $lookup, $unionWith).
    bool fromSubpipeline = false;
};

/**
 * A "dead" field detected by the aliveness analysis, i.e. a field path that was introduced by a
 * pipeline stage but whose value is never used by any downstream stage and does not appear
 * in the pipeline's final output.
 */
struct DeadField {
    /// The stage which introduced this path.
    boost::intrusive_ptr<mongo::DocumentSource> stage;
    /// The field path that was introduced.
    FieldPath path;
};

/**
 * Classifies where a field path came from, resolved by a single hop (see resolveFieldOrigin).
 */
enum class FieldOriginKind : uint8_t {
    /**
     * The field path passed through unchanged from the pipeline input (the base document).
     * The reported input path is the same as the queried path.
     * Examples:
     * 1. {$project: {a: 1}}
     * 2. {$set: {b: 1}}
     *    > the origin of 'a' is {kBaseDocument, inputField="a"}
     */
    kBaseDocument,
    /**
     * The field path is a value-preserving alias of another path (a "simple" rename). The aliased
     * path is reported. Prefix-aliases are also applied.
     * Examples:
     * 1. {$set: {a: '$b'}}
     *    > The origin of 'a' is {kAlias, inputField="b"}
     *    > The origin of 'a.x' is {kAlias, inputField="b.x"}
     *    > The origin of 'a.x.y' is {kAlias, inputField="b.x.y"}
     * 2. {$set: {a: '$b.c'}} (requires canPathBeArray('b') == false to guard array traversal)
     *    > The origin of 'a' is {kAlias, inputField="b.c"}
     * 3. {$set: {'a.b': '$c'}} (requires canPathBeArray('a') == false to guard reshaping)
     *    > The origin of 'a.b' is {kAlias, inputField="c"}
     * If the requirements are not met, the origin kind is reported as kOther.
     */
    kAlias,
    /**
     * The field path is produced by a sub-pipeline (e.g. a $lookup "as" field). The path from the
     * sub-pipeline is reported.
     * Examples:
     * 1. {$lookup: {as: 'a', pipeline: [{$set: 'b'}]}}, {$unwind: 'a'}
     *    > The origin of 'a.b' is {kSubpipeline, inputField='b'}
     *    > The origin of 'a.b.x' is {kSubpipeline, inputField='b.x'}
     */
    kSubpipeline,
    /**
     * The field path is modified in any other way which prevents further origin resolution.
     * This could be because the path is removed, computed, reshaped, required array traversal of
     * the source path and so on.
     */
    kOther,
};

/**
 * Result of resolving a field path's origin.
 */
struct FieldOrigin {
    /// The kind of operation which produced the path in the query.
    FieldOriginKind kind;
    /// The stage which modified the field. nullptr only for kBaseDocument.
    boost::intrusive_ptr<mongo::DocumentSource> modifyingStage;
    /// The prior path which produced the path in the query. none only for kOther.
    boost::optional<std::string> inputField;
};

/**
 * Represents dependencies between fields and stages in a pipeline. Can be partially rebuilt when
 * the pipeline changes.
 */
class DependencyGraph {
public:
    /**
     * Construct a dependency graph covering the entire container.
     */
    explicit DependencyGraph(const DocumentSourceContainer& container,
                             CanPathBeArray canPathBeArray = defaultCanPathBeArray);
    /**
     * Construct a dependency graph covering the range [container.begin(), endIt).
     */
    DependencyGraph(const DocumentSourceContainer& container,
                    DocumentSourceContainer::const_iterator endIt,
                    CanPathBeArray canPathBeArray = defaultCanPathBeArray);
    ~DependencyGraph();
    DependencyGraph(DependencyGraph&&) noexcept;
    DependencyGraph& operator=(DependencyGraph&&) noexcept;

    /**
     * Returns the stage within the pipeline represented by this DependencyGraph instance which last
     * declared, modified or removed the path as seen at the input of 'stage'. If 'stage' is
     * nullptr, returns the stage which last touched the path at the end of the pipeline. Returns
     * nullptr when the path passes through unchanged from the pipeline input.
     *
     * For example, the following stages all modify the path 'a':
     * - {$set: {a: 1}}
     * - {$set: {a.b: 1}}
     * - {$project: {a: 0}}
     * - {$group: {_id: ...}}
     *
     * Note: if this field was last modified by a stage with a sub-pipeline (e.g. $lookup), this
     * does NOT recurse into subpipelines to report which specific subpipeline stage modified this
     * field (if any). If we have a stage like:
     *   {$lookup: {... as: "b", pipeline: [{$set: {x: 12}}]}}
     * querying for field "b.x" after this stage will return the $lookup, NOT the $set.
     */
    boost::intrusive_ptr<mongo::DocumentSource> getPrevModifyingStage(const DocumentSource* stage,
                                                                      PathRef path) const;

    /**
     * Like getPrevModifyingStage, but additionally records the chain of intermediate sub-pipeline
     * containing stages that the path crosses through (i.e. in the example above, will return a
     * pointer to $set).
     *
     * When the path crosses into a sub-pipeline (e.g. "docs.x" through a $lookup), the result has
     * 'fromSubpipeline' set to true and 'srcStages' populated with the chain of intermediate
     * sub-pipeline containing stages followed by the final declaring stage (or nullptr if the path
     * comes from the sub-pipeline's input).
     */
    DeclaringStageResult getPrevModifyingStageIncludingSubpipelines_forTest(
        const DocumentSource* stage, PathRef path) const;

    /**
     * Returns false if the path as seen at the input of 'stage' can be proven to not be an array.
     * Returns true otherwise. If 'stage' is nullptr, the path is evaluated as it appears at the end
     * of the pipeline.
     */
    bool canPathBeArray(const DocumentSource* stage, PathRef path) const;

    /**
     * Returns the constant value of 'path' visible to 'stage' (i.e., as it appears in the input
     * document to 'stage'), if statically known. If 'stage' is nullptr, returns the value visible
     * at the end of the pipeline. Constants are not tracked for fields originating from the
     * pipeline input.
     *
     * A `missing()` Value means the path is provably absent. Returns boost::none when the value is
     * not statically known, which includes the case where resolving the path would have to
     * traverse an array element.
     */
    boost::optional<Value> getConstant(const DocumentSource* stage, PathRef path) const;

    /**
     * Returns the dependency graph for the sub-pipeline of the given stage (e.g. $lookup,
     * $unionWith), or nullptr if the stage has no sub-pipeline.
     */
    const DependencyGraph* getSubpipelineGraph(const DocumentSource* stage) const;

    /**
     * Resolves 'path', as seen at the input of 'stage', back by a single hop toward its origin. If
     * 'stage' is nullptr, the path is resolved as it appears at the end of the pipeline.
     *
     * See FieldOrigin for more information.
     */
    FieldOrigin resolveFieldOrigin(const DocumentSource* stage, PathRef path) const;

    /**
     * Invalidate and recompute the graph from the stage pointed to by 'stageIt' onwards. If
     * 'stageIt' is not given, recomputes the entire graph from the beginning of the container. Only
     * used for testing.
     */
    void recompute_forTest(boost::optional<DocumentSourceContainer::const_iterator> stageIt = {});

    /**
     * Resizes the graph so that it covers the stages in the range [container.begin(), newEndIt).
     * Similarly to std::vector::resize, if the new size is smaller, excess elements are discarded.
     * If the new size is larger, additional elements are initialized. The elements are the stages
     * and their dependency information.
     *
     * 'newEndIt' must be a valid iterator into the underlying container, or the
     * past-the-end iterator. Passing container.begin() resizes the graph to empty.
     * Passing container.end() grows the graph to cover the entire container.
     *
     * This method supports two operations:
     *
     * - Growing - if 'newEndIt' points past the graph's current last stage,
     *   all intermediate stages between the current last stage and the stage preceding
     *   'newEndIt' are read from the container, processed, and appended to the graph.
     *   If the graph is empty, processing starts from the beginning of the container.
     *
     * - Truncating - if 'newEndIt' points to a stage already in the graph (or to
     *   container.begin()), all stages from 'newEndIt' onward are discarded.
     *
     * If the graph already ends just before 'newEndIt', this is a no-op.
     *
     * Important: The caller must ensure that the pipeline stages already represented in the
     * graph have not been modified (e.g. by heuristic rewrites), since they will be retained
     * together with any computed dependency information. If any stage has been modified, first call
     * resize() to truncate the graph back to the last unmodified stage before growing it again with
     * the new stages.
     *
     * Examples:
     *   Suppose the graph currently covers stages [A, B, C] in a container [A, B, C, D, E].
     *
     *   - resize(container.end()) -> grows the graph to [A, B, C, D, E].
     *   - resize(iteratorTo(C)) -> truncates the graph to [A, B].
     *   - resize(iteratorTo(D)) -> no-op, graph already covers [A, B, C].
     *   - resize(container.begin()) -> truncates the graph to empty.
     *
     *   If the graph is empty:
     *   - resize(iteratorTo(D)) -> builds the graph as [A, B, C].
     *
     *   If a rewrite inserts stage X between B and C (container is now [A, B, X, C, D, E]):
     *   - resize(iteratorTo(X)) -> truncates the graph to [A, B], discarding the now-stale C.
     *   - resize(container.end()) -> grows the graph to [A, B, X, C, D, E].
     */
    void resize(DocumentSourceContainer::const_iterator newEndIt);

    /**
     * Returns the set of "dead" fields introduced by single-document transformation stages
     * that are guaranteed to never affect the pipeline output. Deadness is transitive: a field
     * whose only usages are themselves dead is also reported.
     *
     * TODO(SERVER-127212): also walk sub-pipelines and return their dead fields. For now,
     * sub-pipelines are not analyzed; call getSubpipelineGraph(stage)->getDeadFields() to
     * inspect a sub-pipeline.
     */
    std::vector<DeadField> getDeadFields() const;

    /**
     * Renders the graph as a string for debug and golden-test output.
     */
    std::string toDebugString() const;

    /**
     * Renders the graph as BSON for debug and golden-test output.
     */
    BSONObj toBSON() const;

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};

/**
 * Owns and lazily constructs the DependencyGraph for a pipeline and allows it to be invalidated and
 * recomputed as the pipeline is rewritten.
 */
class DependencyGraphContext {
public:
    DependencyGraphContext(ExpressionContext& expCtx, DocumentSourceContainer& container);

    /**
     * Returns a dependency graph that covers the stages from the beginning of the container up to
     * and including 'maxStageIt'. If 'maxStageIt' is not given, covers the whole container.
     */
    const DependencyGraph& getGraph(
        boost::optional<DocumentSourceContainer::const_iterator> maxStageIt = {}) const;

    /**
     * Report that the stages starting at 'startIt' may have changed. The graph will be recomputed
     * for those stages on the next call to getGraph().
     */
    void invalidateFrom(DocumentSourceContainer::const_iterator startIt);

private:
    /**
     * Creates a graph instance.
     */
    std::unique_ptr<DependencyGraph> createGraph(
        DocumentSourceContainer::const_iterator endIt) const;

    ExpressionContext& _expCtx;
    DocumentSourceContainer& _container;

    /**
     * The dependency graph is constructed lazily. In order to be able to hide the implementation
     * details of invalidation and recomputation, we declare the field as mutable and initialise and
     * recompute in the getGraph const accessor.
     */
    mutable std::unique_ptr<DependencyGraph> _graph;
};

}  // namespace mongo::pipeline::dependency_graph
