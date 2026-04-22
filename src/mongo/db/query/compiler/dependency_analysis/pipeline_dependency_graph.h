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


#include "mongo/base/string_data.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <string>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::pipeline::dependency_graph {

using PathRef = StringData;

using CanPathBeArray = std::function<bool(StringData)>;

bool defaultCanPathBeArray(StringData path);

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
     * Return the stage which last modified the path visible from the given DocumentSource. If no
     * DocumentSource is given, returns the stage which last modified the path in the whole
     * pipeline. The stage must have either declared, modified or removed the path. If nullptr, the
     * path is unmodified and assumed to originate from the pipeline input.
     *
     * For example, the following stages all modify the path 'a'.
     * - {$set: {a: 1}}
     * - {$set: {a.b: 1}}
     * - {$project: {a: 0}}
     * - {$group: {_id: ...}}
     */
    boost::intrusive_ptr<mongo::DocumentSource> getDeclaringStage(DocumentSource* stage,
                                                                  PathRef path) const;

    /**
     * Return the stage which last modified the path visible from the given DocumentSource along
     * with all the intermediate stages that contain subpipelines. The stage must have either
     * declared, modified or removed the path. If the stage is nullptr, the path is originating from
     * the pipeline input.
     *
     * When the path crosses into a sub-pipeline (e.g. "docs.x" through a $lookup), the result
     * will have 'fromSubpipeline' set to true and 'srcStages' vector populated with pointers to the
     * sequence of intermediate stages with subpipelines and the final declaring stage or nullptr
     * (if it comes from the collection).
     */
    DeclaringStageResult getDeclaringStageIncludingSubpipelines(DocumentSource* stage,
                                                                PathRef path) const;

    /**
     * Returns false if the path visible from the given DocumentSource can be assumed to not contain
     * arrays. If nullptr, the path is assumed to originate from the pipeline input.
     */
    bool canPathBeArray(DocumentSource* stage, PathRef path) const;

    /**
     * Returns the dependency graph for the sub-pipeline of the given stage (e.g. $lookup,
     * $unionWith), or nullptr if the stage has no sub-pipeline.
     */
    const DependencyGraph* getSubpipelineGraph(DocumentSource* stage) const;

    /**
     * Invalidate and recompute the subgraph starting from the earliest nodes which correspond to
     * the stage pointed to by 'stageIt'.
     */
    void recompute(boost::optional<DocumentSourceContainer::const_iterator> stageIt = {});

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

    std::string toDebugString() const;
    BSONObj toBSON() const;

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};

/**
 * Constructs the DependencyGraph and allows it to be invalidated and recomputed.
 */
class DependencyGraphContext {
public:
    DependencyGraphContext(ExpressionContext& expCtx, DocumentSourceContainer& container);

    /**
     * Get a dependency graph which is valid up to the given element.
     */
    const DependencyGraph& getGraph(
        boost::optional<DocumentSourceContainer::const_iterator> maxStageIt = {}) const;

    /**
     * Report that the stages starting at 'startIt' may have changed.
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
