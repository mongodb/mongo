/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include <algorithm>
#include <boost/optional.hpp>
#include <functional>
#include <map>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_facet.h"
#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/pipeline/pipeline.h"

/**
 * A simple representation of an Aggregation Pipeline and functions for building it.
 * PipelineMetadataTree has no default contents and reflects only the shape of stages. Each stage
 * can be occupied by a a specified type in order to have the tree carry any form of arbitrary
 * metadata. A PipelineMetadataTree is intended to be zipped along with another representation of a
 * pipeline in order to supplement the other representation's metadata.
 */
namespace mongo::pipeline_metadata_tree {

/**
 * An alternate representation of a stage in an Aggregation Pipeline which contains handles to all
 * stages it depends on, forming a tree. Each Stage tracks a specific piece of metadata of type 'T'.
 * Since Stage forms a tree rather than a DAG, there are no handles from $facet component pipelines
 * to their owning $facet stage but there exist pointers from any $facet stage to its component
 * pipelines.
 */
template <typename T>
struct Stage {
    /**
     * Construct an individual Stage from its components.
     */
    Stage(T&& contents,
          std::unique_ptr<Stage> principalChild,
          std::vector<Stage>&& additionalChildren)
        : contents(std::move(contents)),
          principalChild(std::move(principalChild)),
          additionalChildren(std::move(additionalChildren)) {}

    /**
     * Specification of the move constructor intentionally inhibits compiler generation of a copy
     * constructor. This is intentional since accidental copies could be deterimental for
     * performance. This constructor is correctly formed only if the contents type 'T' also has a
     * defined or defaulted move constructor. The correct definition of this constructor is
     * essential for invoking 'makeTree'.
     */
    Stage(Stage&&) = default;

    /**
     * The move assignment operator is subject to the same conditions as the move constructor.
     */
    Stage& operator=(Stage&&) = default;

    /**
     * A comparison operator is correctly defined if the type 'T' has a defined comparison operator.
     * This is optional.
     */
    bool operator==(const Stage& other) const {
        return contents == other.contents &&
            (principalChild && other.principalChild ? *principalChild == *other.principalChild
                                                    : !principalChild && !other.principalChild) &&
            additionalChildren == other.additionalChildren;
    }

    T contents;

    /**
     * The child occuring directly before this stage in the pipeline. This is empty for the first
     * Stage in any pipeline or sub-pipeline.
     */
    std::unique_ptr<Stage> principalChild;

    /**
     * Additional children are the ends of sub-pipelines that feed into this stage. This vector is
     * non-empty only for stages which operate on one or more sub-pipelines such as $facet.
     */
    std::vector<Stage> additionalChildren;
};

template <typename T>
inline auto findStageContents(const NamespaceString& ns,
                              const std::map<NamespaceString, T>& initialStageContents) {
    auto it = initialStageContents.find(ns);
    uassert(51213,
            str::stream() << "Metadata to initialize an aggregation pipeline associated with "
                          << ns.coll() << " is missing.",
            it != initialStageContents.end());
    return it->second;
}

/**
 * Following convention, the nested detail namespace should be treated as private and not accessed
 * directly.
 */
namespace detail {
template <typename T>
std::pair<boost::optional<Stage<T>>, std::function<T(const T&)>> makeTreeWithOffTheEndStage(
    const std::map<NamespaceString, T>& initialStageContents,
    const Pipeline& pipeline,
    const std::function<T(const T&, const std::vector<T>&, const DocumentSource&)>& propagator);

/**
 * Produces additional children to be included in a given Stage if it has sub-pipelines. Included
 * are off-the-end contents that would be generated for those sub-pipelines if they had one
 * additional Stage. A map 'initialStageContents' is provided in order to populate sub-pipelines
 * that, in a graph model, never source from the current pipeline but only feed into it. For
 * example, the initial stage contents are used to seed the contents of $lookup sub-pipelines. The
 * current Stage's contents are provided to copy and populate sub-pipelines that source from the
 * current pipeline and feed back into it through a successive edge. For example, $facet
 * sub-pipelines are populated using a copy of the current Stage's contents.
 */
template <typename T>
inline auto makeAdditionalChildren(
    const std::map<NamespaceString, T>& initialStageContents,
    const DocumentSource& source,
    const std::function<T(const T&, const std::vector<T>&, const DocumentSource&)>& propagator,
    const T& currentContentsToCopyForFacet) {
    std::vector<Stage<T>> children;
    std::vector<T> offTheEndContents;

    if (auto lookupSource = dynamic_cast<const DocumentSourceLookUp*>(&source);
        lookupSource && lookupSource->hasPipeline()) {
        auto [child, offTheEndReshaper] = makeTreeWithOffTheEndStage(
            initialStageContents, lookupSource->getResolvedIntrospectionPipeline(), propagator);
        offTheEndContents.push_back(offTheEndReshaper(child.get().contents));
        children.push_back(std::move(*child));
    }
    if (auto facetSource = dynamic_cast<const DocumentSourceFacet*>(&source))
        std::transform(facetSource->getFacetPipelines().begin(),
                       facetSource->getFacetPipelines().end(),
                       std::back_inserter(children),
                       [&](const auto& fPipe) {
                           auto [child, offTheEndReshaper] = makeTreeWithOffTheEndStage(
                               initialStageContents, *fPipe.pipeline, propagator);
                           offTheEndContents.push_back(offTheEndReshaper(child.get().contents));
                           return std::move(*child);
                       });
    return std::pair(std::move(children), std::move(offTheEndContents));
}

/**
 * Produces a stage and returns a function to determine the contents for the next Stage. Given are
 * an optional reference to a previous stage which is disengaged at the start of a pipeline or sub-
 * pipeline. Also given is 'reshapeContents', a function to produce the content of the current
 * stage. The current DocumentSource to build a corresponding Stage for is given through 'source'.
 * If there is no previous Stage, the entry from the 'initialStageContents' matching the current
 * namespace is used to populate the new Stage. If the entry is missing, an exception is thrown.
 */
template <typename T>
inline auto makeStage(
    const std::map<NamespaceString, T>& initialStageContents,
    boost::optional<Stage<T>>&& previous,
    const std::function<T(const T&)>& reshapeContents,
    const DocumentSource& source,
    const std::function<T(const T&, const std::vector<T>&, const DocumentSource&)>& propagator) {
    auto contents = (previous) ? reshapeContents(previous.get().contents)
                               : findStageContents(source.getContext()->ns, initialStageContents);

    auto [additionalChildren, offTheEndContents] =
        makeAdditionalChildren(initialStageContents, source, propagator, contents);

    auto principalChild = previous ? std::make_unique<Stage<T>>(std::move(previous.get()))
                                   : std::unique_ptr<Stage<T>>();
    std::function<T(const T&)> reshaper(
        [&, offTheEndContents{std::move(offTheEndContents)}](const T& reshapable) {
            return propagator(reshapable, offTheEndContents, source);
        });
    return std::pair(
        boost::optional<Stage<T>>(
            Stage(std::move(contents), std::move(principalChild), std::move(additionalChildren))),
        std::move(reshaper));
}

template <typename T>
inline std::pair<boost::optional<Stage<T>>, std::function<T(const T&)>> makeTreeWithOffTheEndStage(
    const std::map<NamespaceString, T>& initialStageContents,
    const Pipeline& pipeline,
    const std::function<T(const T&, const std::vector<T>&, const DocumentSource&)>& propagator) {
    std::pair<boost::optional<Stage<T>>, std::function<T(const T&)>> stageAndReshapeContents;
    for (const auto& source : pipeline.getSources())
        stageAndReshapeContents = makeStage(initialStageContents,
                                            std::move(stageAndReshapeContents.first),
                                            stageAndReshapeContents.second,
                                            *source,
                                            propagator);
    return stageAndReshapeContents;
}

template <typename T>
inline void walk(Stage<T>* stage,
                 Pipeline::SourceContainer::iterator* sourceIter,
                 const std::function<void(Stage<T>*, DocumentSource*)>& zipper) {
    if (stage->principalChild)
        walk(stage->principalChild.get(), sourceIter, zipper);

    if (auto lookupSource = dynamic_cast<DocumentSourceLookUp*>(&***sourceIter);
        lookupSource && lookupSource->hasPipeline()) {
        auto iter = lookupSource->getResolvedIntrospectionPipeline().getSources().begin();
        walk(&stage->additionalChildren.front(), &iter, zipper);
    }

    if (auto facetSource = dynamic_cast<const DocumentSourceFacet*>(&***sourceIter)) {
        auto facetIter = facetSource->getFacetPipelines().begin();
        for (auto& child : stage->additionalChildren) {
            auto iter = facetIter++->pipeline->getSources().begin();
            walk(&child, &iter, zipper);
        }
    }

    zipper(stage, &**(*sourceIter)++);
}

}  // namespace detail

/**
 * Builds a Stage from a Pipeline. Initial contents for the first pipline stage must be provided. A
 * function 'propagator' is neccesary to determine how to build the contents of all further stages.
 * A stage will receive the built contents from its directly preceding stage. Initial contents must
 * be placed in 'initialStageContents'. Any expressive lookup pipelines require an additional
 * initial content element in this queue.
 *
 * The arguments to propagator will be actualized with the following:
 * 'T&' - In general, the contents from the previous stage, initial stages of the main pipeline and
 * $lookup pipelines receive an element off the queue 'initialStageContents'. $facet receives a copy
 * of its parent's contents.
 * 'std::vector<T>&' - Completed contents from sub-pipelines. $facet's additional children and
 * expressive $lookup's final contents will be manifested in here. Note that these will be
 * "off-the-end", that is constructed from the final stage of a sub-pipeline and not actually
 * contained in that pipeline. This vector is empty for most stages which have only one child.
 * 'DocumentSource&' - the current stage of the 'pipeline' a Stage object is being built for.
 *
 * Returns the final Stage<T> of the pipeline along with the final off-the-end metadata T from
 * calling the 'propagator' function on the last source.
 */
template <typename T>
inline std::pair<boost::optional<Stage<T>>, T> makeTree(
    const std::map<NamespaceString, T>& initialStageContents,
    const Pipeline& pipeline,
    const std::function<T(const T&, const std::vector<T>&, const DocumentSource&)>& propagator) {
    // For empty pipelines, there's no Stage<T> to return and the output schema is the same as the
    // input schema.
    if (pipeline.getSources().empty()) {
        return std::pair(boost::none,
                         findStageContents(pipeline.getContext()->ns, initialStageContents));
    }

    auto&& [finalStage, reshaper] =
        detail::makeTreeWithOffTheEndStage(initialStageContents, pipeline, propagator);

    return std::pair(std::move(*finalStage), reshaper(finalStage.get().contents));
}

/**
 * Walk a PipelineMetadataTree along with a Pipeline. Passes each Stage and its corresponding
 * DocumentSource to 'zipper' two-by-two.
 */
template <typename T>
inline void zip(Stage<T>* tree,
                Pipeline* pipeline,
                const std::function<void(Stage<T>*, DocumentSource*)>& zipper) {
    auto iter = pipeline->getSources().begin();
    detail::walk(tree, &iter, zipper);
}

}  // namespace mongo::pipeline_metadata_tree
