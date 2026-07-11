// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/op_debug.h"
#include "mongo/db/pipeline/catalog_resource_handle.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/search/document_source_internal_search_id_lookup_gen.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/search/search_query_view_spec_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

class DSInternalSearchIdLookUpCatalogResourceHandle;
/**
 * Queries local collection for _id equality matches. Intended for use with
 * $_internalSearchMongotRemote (see $search) as part of the Search project.
 *
 * Input documents will be ignored and skipped if they do not have a value at field "_id".
 * Input documents will be ignored and skipped if no document with key specified at "_id"
 * is locally-stored.
 */
class DocumentSourceInternalSearchIdLookUp final : public DocumentSource {
public:
    static constexpr std::string_view kStageName = "$_internalSearchIdLookup"sv;

    DocumentSourceInternalSearchIdLookUp(DocumentSourceIdLookupSpec spec,
                                         const boost::intrusive_ptr<ExpressionContext>& expCtx);

    std::string_view getSourceName() const final;

    static const Id& id;

    Id getId() const override {
        return id;
    }

    StageConstraints constraints(PipelineSplitState pipeState) const override {
        StageConstraints constraints(StreamType::kStreaming,
                                     PositionRequirement::kNone,
                                     HostTypeRequirement::kTargetedShards,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kNotAllowed,
                                     TransactionRequirement::kNotAllowed,
                                     LookupRequirement::kAllowed,
                                     UnionRequirement::kAllowed,
                                     ChangeStreamRequirement::kDenylist);
        // Set to true to allow this to be run on the shards before the search implicit sort.
        constraints.preservesOrderAndMetadata = true;
        // All search stages are unsupported on timeseries collections.
        constraints.canRunOnTimeseries = false;
        constraints.outputDependsOnSingleInput = true;

        return constraints;
    }

    DepsTracker::State getDependencies(DepsTracker* deps) const final {
        // This just depends on the '_id' field.
        deps->fields.insert("_id");
        return DepsTracker::State::SEE_NEXT;
    }
    /**
     * Serialize this stage - return is of the form { $_internalSearchIdLookup: {} }
     */
    Value serialize(const query_shape::SerializationOptions& opts =
                        query_shape::SerializationOptions{}) const final;

    /**
     * This stage must be run on each shard, but that must be enforced at a higher-level in the
     * pipeline-splitting logic.
     *
     * For the purposes of this function, we want default behavior to happen upon seeing an idLookup
     * (which is to push it down to the shards and continue forward in looking for a split point).
     */
    boost::optional<DistributedPlanLogic> distributedPlanLogic(
        const DistributedPlanContext* ctx) final {
        return boost::none;
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

    /**
     * SearchIdLookupMetrics are shared state between InternalSearchIdLookup and predecessor
     * $search/$vectorSearch stages that consume the metrics. They are defined and made available on
     * OpDebug as well, for easier sharing with extension stages, but IdLookup owns the metrics to
     * ensure that they can be aggregated across the whole operation.
     */
    using SearchIdLookupMetrics = OpDebug::SearchIdLookupMetrics;

    std::shared_ptr<SearchIdLookupMetrics> getSearchIdLookupMetrics() {
        return _searchIdLookupMetrics;
    }

    void bindCatalogInfo(
        const MultipleCollectionAccessor& collections,
        boost::intrusive_ptr<ShardRoleTransactionResourcesStasherForPipeline> sharedStasher) final;

    DocumentSourceContainer::iterator optimizeAt(DocumentSourceContainer::iterator itr,
                                                 DocumentSourceContainer* container);

private:
    friend boost::intrusive_ptr<exec::agg::Stage> documentSourceInternalSearchIdLookupToStageFn(
        const boost::intrusive_ptr<DocumentSource>&);

    DocumentSourceIdLookupSpec _spec;

    // Handle to catalog state. Also contains the collection needed for execution.
    boost::intrusive_ptr<DSInternalSearchIdLookUpCatalogResourceHandle> _catalogResourceHandle;

    std::shared_ptr<SearchIdLookupMetrics> _searchIdLookupMetrics =
        std::make_shared<SearchIdLookupMetrics>();
};

class DSInternalSearchIdLookUpCatalogResourceHandle : public DSCatalogResourceHandleBase {
public:
    DSInternalSearchIdLookUpCatalogResourceHandle(
        boost::intrusive_ptr<ShardRoleTransactionResourcesStasherForPipeline> stasher,
        CollectionAcquisition collection)
        : DSCatalogResourceHandleBase(std::move(stasher)), _collection(std::move(collection)) {}

    CollectionAcquisition getCollection() {
        tassert(11140101,
                "catalogResourceHandle must be acquired to access the collection",
                isAcquired());
        return _collection;
    }

    /**
     * Returns the upfront acquisition for building a PreAcquiredCollectionAcquirer. Unlike
     * getCollection(), this does not require the handle to be acquired onto the opCtx.
     */
    CollectionAcquisition getCollectionForLookupExecutor() const {
        return _collection;
    }

private:
    CollectionAcquisition _collection;
};

}  // namespace mongo
