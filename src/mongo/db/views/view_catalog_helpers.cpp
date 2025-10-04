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

#include "mongo/db/views/view_catalog_helpers.h"

#include <absl/container/node_hash_map.h>
#include <absl/container/node_hash_set.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/db/curop.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/process_interface/stub_mongo_process_interface.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/query/collation/collation_spec.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/server_options.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/views/view_graph.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"
#include "mongo/util/uuid.h"
#include "mongo/util/version/releases.h"

#include <algorithm>
#include <iterator>
#include <list>
#include <utility>
#include <vector>

namespace mongo {
namespace view_catalog_helpers {

StatusWith<stdx::unordered_set<NamespaceString>> validatePipeline(OperationContext* opCtx,
                                                                  const ViewDefinition& viewDef) {
    const LiteParsedPipeline liteParsedPipeline(viewDef.viewOn(), viewDef.pipeline());

    // The API version pipeline validation should be skipped for time-series view because of
    // following reasons:
    //     - the view pipeline is not created by (or visible to) the end-user and should be skipped.
    //     - the view pipeline can have stages that are not allowed in stable API version '1' eg.
    //       '$_internalUnpackBucket'.
    bool performApiVersionChecks = !viewDef.timeseries();
    liteParsedPipeline.validate(opCtx, performApiVersionChecks);
    liteParsedPipeline.checkStagesAllowedInViewDefinition();

    // Verify that this is a legitimate pipeline specification by making sure it parses
    // correctly. In order to parse a pipeline we need to resolve any namespaces involved to a
    // collection and a pipeline, but in this case we don't need this map to be accurate since
    // we will not be evaluating the pipeline.
    ResolvedNamespaceMap resolvedNamespaces;

    // Create copy of involved namespaces, as these can be moved into the result.
    for (const auto& nss : liteParsedPipeline.getInvolvedNamespaces()) {
        resolvedNamespaces[nss] = {nss, {}};
    }
    AggregateCommandRequest aggregateRequest(viewDef.viewOn(), viewDef.pipeline());
    // We can use a stub MongoProcessInterface because we are only
    // parsing the Pipeline for validation here. We won't do anything
    // with the pipeline that will require a real implementation.
    auto expCtx = ExpressionContextBuilder{}
                      .fromRequest(opCtx, aggregateRequest)
                      .collator(CollatorInterface::cloneCollator(viewDef.defaultCollator()))
                      .resolvedNamespace(std::move(resolvedNamespaces))
                      .mayDbProfile(true)
                      // The pipeline parser needs to know that we're parsing a pipeline for a view
                      // definition to apply some additional checks.
                      .isParsingViewDefinition(true)
                      .build();

    try {
        auto pipeline =
            Pipeline::parse(viewDef.pipeline(), std::move(expCtx), [&](const Pipeline& pipeline) {
                // Validate that the view pipeline does not contain any ineligible stages.
                const auto& sources = pipeline.getSources();
                const auto firstPersistentStage =
                    std::find_if(sources.begin(), sources.end(), [](const auto& source) {
                        return source->constraints().writesPersistentData();
                    });

                uassert(ErrorCodes::OptionNotSupportedOnView,
                        str::stream()
                            << "The aggregation stage "
                            << firstPersistentStage->get()->getSourceName() << " in location "
                            << std::distance(sources.begin(), firstPersistentStage)
                            << " of the pipeline cannot be used in the view definition of "
                            << viewDef.name().toStringForErrorMsg() << " because it writes to disk",
                        firstPersistentStage == sources.end());

                uassert(ErrorCodes::OptionNotSupportedOnView,
                        "$changeStream cannot be used in a view definition",
                        sources.empty() || !sources.front()->constraints().isChangeStreamStage());

                std::for_each(sources.begin(), sources.end(), [](auto& stage) {
                    uassert(ErrorCodes::InvalidNamespace,
                            str::stream() << "'" << stage->getSourceName()
                                          << "' cannot be used in a view definition",
                            !stage->constraints().isIndependentOfAnyCollection);
                });
            });
    } catch (const DBException& ex) {
        return ex.toStatus();
    }

    return liteParsedPipeline.getInvolvedNamespaces();
}

NamespaceString findSourceCollectionNamespace(OperationContext* opCtx,
                                              std::shared_ptr<const CollectionCatalog> catalog,
                                              const NamespaceString& nss) {

    // Points to the name of the most resolved namespace.
    const NamespaceString* resolvedNss = &nss;

    int depth = 0;
    for (; depth < ViewGraph::kMaxViewDepth; depth++) {
        auto view = catalog->lookupView(opCtx, *resolvedNss);
        if (!view) {
            return NamespaceString({*resolvedNss});
        }
        resolvedNss = &view->viewOn();
    }

    MONGO_UNREACHABLE;
}


StatusWith<ResolvedView> resolveView(OperationContext* opCtx,
                                     std::shared_ptr<const CollectionCatalog> catalog,
                                     const NamespaceString& nss,
                                     boost::optional<BSONObj> timeSeriesCollator) {
    // Points to the name of the most resolved namespace.
    const NamespaceString* resolvedNss = &nss;

    // Holds the combination of all the resolved views.
    std::vector<BSONObj> resolvedPipeline;

    // If the catalog has not been tampered with, all views seen during the resolution will have
    // the same collation. As an optimization, we fill out the collation spec only once.
    boost::optional<BSONObj> collation;

    // The last seen view definition, which owns the NamespaceString pointed to by
    // 'resolvedNss'.
    std::shared_ptr<ViewDefinition> lastViewDefinition;

    std::vector<NamespaceString> dependencyChain{nss};

    int depth = 0;
    boost::optional<bool> mixedData = boost::none;
    boost::optional<TimeseriesOptions> tsOptions = boost::none;
    boost::optional<bool> hasExtendedRange = boost::none;
    boost::optional<bool> fixedBuckets = boost::none;

    // Whether we are working with a new, viewless timeseries collection. In general, we expect this
    // to be false, but this is present so that we can enforce this invariant in ResolvedView. Once
    // this parameter is removed from the catalog cache, this variable should be removed as well.
    bool isNewTimeseriesWithoutView = false;

    for (; depth < ViewGraph::kMaxViewDepth; depth++) {
        auto view = catalog->lookupView(opCtx, *resolvedNss);
        if (!view) {
            // Return error status if pipeline is too large.
            int pipelineSize = 0;
            for (const auto& obj : resolvedPipeline) {
                pipelineSize += obj.objsize();
            }
            if (pipelineSize > ViewGraph::kMaxViewPipelineSizeBytes) {
                return {ErrorCodes::ViewPipelineMaxSizeExceeded,
                        str::stream() << "View pipeline exceeds maximum size; maximum size is "
                                      << ViewGraph::kMaxViewPipelineSizeBytes};
            }

            auto curOp = CurOp::get(opCtx);
            curOp->debug().addResolvedViews(dependencyChain, resolvedPipeline);

            return StatusWith<ResolvedView>(
                {*resolvedNss,
                 std::move(resolvedPipeline),
                 collation ? std::move(collation.value()) : CollationSpec::kSimpleSpec,
                 tsOptions,
                 mixedData,
                 hasExtendedRange,
                 fixedBuckets,
                 isNewTimeseriesWithoutView});
        }

        resolvedNss = &view->viewOn();

        if (storageGlobalParams.restore) {
            // During a selective restore procedure, skip checking options as the collection may no
            // longer exist.
            continue;
        }

        if (view->timeseries()) {
            auto tsCollection = catalog->lookupCollectionByNamespace(opCtx, *resolvedNss);
            uassert(6067201,
                    str::stream() << "expected time-series buckets collection "
                                  << (*resolvedNss).toStringForErrorMsg() << " to exist",
                    tsCollection);
            mixedData = tsCollection->getTimeseriesMixedSchemaBucketsState()
                            .mustConsiderMixedSchemaBucketsInReads();
            tsOptions = tsCollection->getTimeseriesOptions();
            hasExtendedRange = tsCollection->getRequiresTimeseriesExtendedRangeSupport();
            fixedBuckets = tsCollection->areTimeseriesBucketsFixed();
            isNewTimeseriesWithoutView = tsCollection->isNewTimeseriesWithoutView();
        }

        dependencyChain.push_back(*resolvedNss);
        if (!collation) {
            if (timeSeriesCollator) {
                collation = *timeSeriesCollator;
            } else {
                collation = view->defaultCollator() ? view->defaultCollator()->getSpec().toBSON()
                                                    : CollationSpec::kSimpleSpec;
            }
        }

        // Prepend the underlying view's pipeline to the current working pipeline.
        const std::vector<BSONObj>& toPrepend = view->pipeline();
        resolvedPipeline.insert(resolvedPipeline.begin(), toPrepend.begin(), toPrepend.end());

        // If the first stage is a $collStats, then we return early with the viewOn namespace.
        if (toPrepend.size() > 0 && !toPrepend[0]["$collStats"].eoo()) {
            auto curOp = CurOp::get(opCtx);
            curOp->debug().addResolvedViews(dependencyChain, resolvedPipeline);

            return StatusWith<ResolvedView>(
                {*resolvedNss, std::move(resolvedPipeline), std::move(collation.value())});
        }
    }

    if (depth >= ViewGraph::kMaxViewDepth) {
        return {ErrorCodes::ViewDepthLimitExceeded,
                str::stream() << "View depth too deep or view cycle detected; maximum depth is "
                              << ViewGraph::kMaxViewDepth};
    }

    MONGO_UNREACHABLE;
}

}  // namespace view_catalog_helpers
}  // namespace mongo
