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

#include "mongo/db/local_catalog/clustered_collection_options_gen.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/query/canonical_distinct.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/collation/collation_index_key.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/compiler/metadata/index_entry.h"
#include "mongo/db/query/compiler/stats/collection_statistics.h"
#include "mongo/db/query/index_hint.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/query_knobs_gen.h"

#include <vector>

namespace mongo {

/**
 * Struct containing basic stats about a collection useful for query planning.
 */
struct PlannerCollectionInfo {
    // The number of records in the collection.
    long long noOfRecords{0};

    // The approximate size of the collection in bytes.
    long long approximateDataSizeBytes{0};

    // The allocated storage size in bytes.
    long long storageSizeBytes{0};

    // Whether this is a timeseries collection. This is sometimes used in planning decisions.
    bool isTimeseries = false;
};

/**
 * Struct containing information about collections (such as the 'from' collection in
 * $lookup) useful for query planning.
 */
struct CollectionInfo {
    CollectionInfo() = default;
    CollectionInfo(const CollectionInfo&) = delete;
    CollectionInfo& operator=(const CollectionInfo&) = delete;
    CollectionInfo(CollectionInfo&& other) noexcept = default;
    CollectionInfo& operator=(CollectionInfo&&) noexcept = default;

    // See QueryPlannerParams::Options.
    // For secondary collections, this is currently unused (but may still be populated).
    size_t options{0 /* DEFAULT */};

    // Indexes available for planning.
    std::vector<IndexEntry> indexes{};

    bool exists{true};

    // Basic collection stats for a given collection.
    PlannerCollectionInfo stats{};

    // TODO SERVER-85321 Centralize the COLLSCAN direction hinting mechanism.
    // Optional hint for specifying the allowed collection scan direction. Unlike cursor '$natural'
    // hints, this does not force the planner to prefer collection scans over other candidate
    // solutions. This is currently used for applying query settings '$natural' hints.
    boost::optional<NaturalOrderHint::Direction> collscanDirection = boost::none;

    // Histogram-based statistics for fields in the collection.
    std::unique_ptr<stats::CollectionStatistics> collStats{nullptr};
};


// This holds information about the internal traversal preference used for time series. If we choose
// an index that involves fields we're interested in, we prefer a specific direction to avoid a
// blocking sort.
struct TraversalPreference {
    // If we end up with an index that provides {sortPattern}, we prefer to scan it in direction
    // {direction}.
    BSONObj sortPattern;
    int direction;
    // Cluster key for the collection this query accesses (for time-series it's control.min.time).
    // If a collection scan is chosen, this will be compared against the sortPattern to see if we
    // can satisfy the traversal preference.
    std::string clusterField;
};

struct QueryPlannerParams {
    enum Options {
        // You probably want to set this.
        DEFAULT = 0,

        // Set this if you don't want a table scan.
        // See http://docs.mongodb.org/manual/reference/parameters/
        // Beware: setting only this option can still lead to 'CLUSTERED_IXSCAN' plans. To avoid
        // creating such plans, the option 'STRICT_NO_TABLE_SCAN' should be used together with this
        // option.
        NO_TABLE_SCAN = 1,

        // Set this if you *always* want a collscan outputted, even if there's an ixscan.  This
        // makes ranking less accurate, especially in the presence of blocking stages.
        INCLUDE_COLLSCAN = 1 << 1,

        // Set this if you're running on a sharded cluster.  We'll add a "drop all docs that
        // shouldn't be on this shard" stage before projection.
        //
        // In order to set this, you must check OperationShardingState::isComingFromRouter() in the
        // same lock that you use to build the query executor. You must also wrap the PlanExecutor
        // in a ClientCursor within the same lock.
        //
        // See the comment on ShardFilterStage for details.
        INCLUDE_SHARD_FILTER = 1 << 2,

        // Set this if you want to turn on index intersection.
        INDEX_INTERSECTION = 1 << 3,

        // Set this to generate covered whole IXSCAN plans.
        GENERATE_COVERED_IXSCANS = 1 << 4,

        // Set this to track the most recent timestamp seen by this cursor while scanning the
        // oplog or change collection.
        TRACK_LATEST_OPLOG_TS = 1 << 5,

        // Set this so that collection scans on the oplog wait for visibility before reading.
        OPLOG_SCAN_WAIT_FOR_VISIBLE = 1 << 6,

        // Set this so that tryGetQuerySolutionForDistinct() will only use a plan that _guarantees_
        // it will return exactly one document per value of the distinct field. See the comments
        // above the declaration of tryGetQuerySolutionForDistinct() for more detail.
        STRICT_DISTINCT_ONLY = 1 << 7,

        // Set this on an oplog scan to uassert that the oplog has not already rolled over the
        // minimum 'ts' timestamp specified in the query.
        ASSERT_MIN_TS_HAS_NOT_FALLEN_OFF_OPLOG = 1 << 8,

        // Instruct the plan enumerator to enumerate contained $ors in a special order. $or
        // enumeration can generate an exponential number of plans, and is therefore limited at some
        // arbitrary cutoff controlled by a parameter. When this limit is hit, the order of
        // enumeration is important. For example, a query like the following has a "contained $or"
        // (within an $and):
        // {a: 1, $or: [{b: 1, c: 1}, {b: 2, c: 2}]}
        // For this query if there are indexes a_b={a: 1, b: 1} and a_c={a: 1, c: 1}, the normal
        // enumeration order would output assignments [a_b, a_b], [a_c, a_b], [a_b, a_c], then [a_c,
        // a_c]. This flag will instruct the enumerator to instead prefer a different order. It's
        // hard to summarize, but perhaps the phrases "lockstep enumeration", "simultaneous
        // advancement", or "parallel iteration" will help the reader. The effect is to give earlier
        // enumeration to plans which use the same index of alternative across all branches. In this
        // order, we would get assignments [a_b, a_b], [a_c, a_c], [a_c, a_b], then [a_b, a_c]. This
        // is thought to be helpful in general, but particularly in cases where all children of the
        // $or use the same fields and have the same indexes available, as in this example.
        ENUMERATE_OR_CHILDREN_LOCKSTEP = 1 << 9,

        // Ensure that any plan generated returns data that is "owned." That is, all BSONObjs are
        // in an "owned" state and are not pointing to data that belongs to the storage engine.
        RETURN_OWNED_DATA = 1 << 10,

        // When generating column scan queries, splits match expressions so that the filters can be
        // applied per-column. This is off by default, since the execution side doesn't support it
        // yet.
        GENERATE_PER_COLUMN_FILTERS = 1 << 11,

        // This is an extension to the NO_TABLE_SCAN parameter. This stricter option will also
        // avoid a CLUSTERED_IXSCAN which comes built into a collection scan when the collection is
        // clustered.
        STRICT_NO_TABLE_SCAN = 1 << 12,

        // Set this to ignore the query settings imposed constraints over plan selection.
        IGNORE_QUERY_SETTINGS = 1 << 13,

        // Set this if you want the planner to generate a QSN that will be compatible with the
        // SBE stage builder.
        TARGET_SBE_STAGE_BUILDER = 1 << 14,
    };

    /**
     * Struct containing all the arguments that are required for QueryPlannerParams initialization
     * for distinct commands.
     */
    struct ArgsForDistinct {
        OperationContext* opCtx;
        const CanonicalQuery& canonicalQuery;
        const MultipleCollectionAccessor& collections;
        size_t plannerOptions;
        bool flipDistinctScanDirection;
    };

    /**
     * Struct containing all the arguments that are required for QueryPlannerParams initialization
     * for express commands.
     */
    struct ArgsForExpress {
        OperationContext* opCtx;
        const CanonicalQuery& canonicalQuery;
        const MultipleCollectionAccessor& collections;
        size_t plannerOptions;
    };

    /**
     * Struct containing all the arguments required for initializing QueryPlannerParams for commands
     * using the single collection queries. QueryPlannerParams can then be upgraded to support
     * multiple collection ones as well by calling 'fillOutSecondaryCollectionsPlannerParams()'.
     * This can only be done after SBE stages have been pushed down to canonical query.
     */
    struct ArgsForSingleCollectionQuery {
        OperationContext* opCtx;
        const CanonicalQuery& canonicalQuery;
        const MultipleCollectionAccessor& collections;
        size_t plannerOptions = DEFAULT;
        boost::optional<TraversalPreference> traversalPreference = boost::none;
        QueryPlanRankerModeEnum planRankerMode = QueryPlanRankerModeEnum::kMultiPlanning;
    };

    /**
     * Struct containing the necessary arguments for initializing QueryPlannerParams for internal
     * shard key queries.
     */
    struct ArgsForInternalShardKeyQuery {
        size_t plannerOptions;
        IndexEntry indexEntry;
    };

    struct ArgsForTest {};

    /**
     * Initializes query planner parameters by fetching relevant indexes and applying query
     * settings, index filters.
     */
    explicit QueryPlannerParams(ArgsForDistinct&& args);

    /**
     * Initializes query planner parameters by filling collection info and shard filter.
     */
    explicit QueryPlannerParams(const ArgsForExpress& args) : providedOptions(args.plannerOptions) {
        mainCollectionInfo.options = args.plannerOptions;
        fillOutPlannerParamsForExpressQuery(
            args.opCtx, args.canonicalQuery, args.collections.getMainCollection());
    }

    /**
     * Initializes query planner parameters by filling in main collection info and fetching main
     * collection indexes.
     */
    explicit QueryPlannerParams(ArgsForSingleCollectionQuery&& args)
        : providedOptions(args.plannerOptions),
          traversalPreference(std::move(args.traversalPreference)),
          planRankerMode(args.planRankerMode) {
        mainCollectionInfo.options = args.plannerOptions;
        if (!args.collections.hasMainCollection()) {
            return;
        }
        // Prevent histogramCE on queries on internal collections. This is because currently
        // histogramCE will cause queries to fail if the query contains a predicate on a field
        // without a histogram. We don't create histograms on internal collections. To prevent such
        // queries from failing, we use multiplanning in this case.
        if (planRankerMode == QueryPlanRankerModeEnum::kHistogramCE &&
            args.canonicalQuery.nss().dbName().isInternalDb()) {
            planRankerMode = QueryPlanRankerModeEnum::kMultiPlanning;
        }
        fillOutPlannerParamsForExpressQuery(
            args.opCtx, args.canonicalQuery, args.collections.getMainCollection());
        fillOutMainCollectionPlannerParams(
            args.opCtx, args.canonicalQuery, args.collections, args.planRankerMode);
    }

    /**
     * Initializes query planner parameters by simply inserting the provided index entry.
     */
    explicit QueryPlannerParams(ArgsForInternalShardKeyQuery&& args)
        : providedOptions(args.plannerOptions) {
        mainCollectionInfo.options = args.plannerOptions;
        mainCollectionInfo.indexes.push_back(std::move(args.indexEntry));
    }

    explicit QueryPlannerParams(ArgsForTest&& args) {
        mainCollectionInfo.options = DEFAULT;
    }

    QueryPlannerParams(const QueryPlannerParams&) = delete;
    QueryPlannerParams& operator=(const QueryPlannerParams& other) = delete;

    QueryPlannerParams(QueryPlannerParams&&) = default;
    QueryPlannerParams& operator=(QueryPlannerParams&& other) = default;

    /**
     * Fills planner parameters for the secondary collections.
     */
    void fillOutSecondaryCollectionsPlannerParams(OperationContext* opCtx,
                                                  const CanonicalQuery& canonicalQuery,
                                                  const MultipleCollectionAccessor& collections);

    /**
     * This method updates this QueryPlannerParams object as needed so that it can be used with
     * the SBE engine.
     */
    void setTargetSbeStageBuilder(OperationContext* opCtx,
                                  const CanonicalQuery& canonicalQuery,
                                  const MultipleCollectionAccessor& collections);

    /**
     * If query supports index filters, filters params.indices according to the configuration. In
     * addition, sets that there were index filters.
     * NOTE: method is marked public for testing purposes only.
     */
    void applyIndexFilters(const CanonicalQuery& canonicalQuery, const CollectionPtr& collection);

    // Options as provided when constructing the params, without any collection specific
    // modifications.
    size_t providedOptions = DEFAULT;

    CollectionInfo mainCollectionInfo;

    // What's our shard key?  If INCLUDE_SHARD_FILTER is set we will create a shard filtering
    // stage.  If we know the shard key, we can perform covering analysis instead of always
    // forcing a fetch.
    BSONObj shardKey;

    // What's the max number of indexed solutions we want to output?  It's expensive to compare
    // plans via the MultiPlanStage, and the set of possible plans is very large for certain
    // index+query combinations.
    size_t maxIndexedSolutions = internalQueryPlannerMaxIndexedSolutions.load();

    // Specifies the clusteredIndex information necessary to utilize the cluster key in bounded
    // collection scans and other query operations.
    boost::optional<ClusteredCollectionInfo> clusteredInfo = boost::none;

    // Specifies the collator information necessary to utilize the cluster key in bounded
    // collection scans and other query operations.
    const CollatorInterface* clusteredCollectionCollator = nullptr;

    // List of information about any secondary collections that can be executed against.
    std::map<NamespaceString, CollectionInfo> secondaryCollectionsInfo;

    boost::optional<TraversalPreference> traversalPreference = boost::none;

    // Size of available memory in bytes.
    long long availableMemoryBytes{0};

    // TODO: SERVER-88503 Remove Index Filters feature.
    // Were index filters applied to indices?
    bool indexFiltersApplied{false};

    // Were query settings applied?
    bool querySettingsApplied{false};

    QueryPlanRankerModeEnum planRankerMode = QueryPlanRankerModeEnum::kMultiPlanning;

private:
    bool requiresShardFiltering(const CanonicalQuery& canonicalQuery,
                                const CollectionPtr& collection);

    MONGO_COMPILER_ALWAYS_INLINE
    void fillOutPlannerParamsForExpressQuery(OperationContext* opCtx,
                                             const CanonicalQuery& canonicalQuery,
                                             const CollectionPtr& collection) {
        if (requiresShardFiltering(canonicalQuery, collection)) {
            // This query may have been issued to multiple shards.
            // Knowing the shardKey may avoid fetching the document to apply shard filtering
            // e.g., if an ixscan will provide all required fields.
            shardKey = collection.getShardKeyPattern().toBSON();
        } else {
            // A shard filter was not requested, or is not required - clear the flag.
            mainCollectionInfo.options &= ~QueryPlannerParams::INCLUDE_SHARD_FILTER;
        }

        if (collection->isClustered()) {
            clusteredInfo = collection->getClusteredInfo();
            clusteredCollectionCollator = collection->getDefaultCollator();
        }
    }

    /**
     * Fills planner parameters for the main collection.
     */
    void fillOutMainCollectionPlannerParams(OperationContext* opCtx,
                                            const CanonicalQuery& canonicalQuery,
                                            const MultipleCollectionAccessor& collections,
                                            QueryPlanRankerModeEnum planRankerMode);

    /**
     * Applies query settings to the main collection if applicable. If not, tries to apply index
     * filters.
     */
    void applyQuerySettingsOrIndexFiltersForMainCollection(
        const CanonicalQuery& canonicalQuery, const MultipleCollectionAccessor& collections);

    /**
     * Applies 'indexHints' query settings for the given 'collection'.In addition, sets that there
     * were query settings applied.
     */
    void applyQuerySettingsForCollection(
        const CanonicalQuery& canonicalQuery,
        const NamespaceString& nss,
        const query_settings::IndexHintSpecs& indexHintSpecs,
        CollectionInfo& collectionInfo,
        const boost::optional<TimeseriesOptions>& timeseriesOptions);

    void applyQuerySettingsIndexHintsForCollection(const CanonicalQuery& canonicalQuery,
                                                   const std::vector<mongo::IndexHint>& indexHints,
                                                   std::vector<IndexEntry>& indexes);

    void applyQuerySettingsNaturalHintsForCollection(
        const CanonicalQuery& canonicalQuery,
        const std::vector<mongo::IndexHint>& indexHints,
        CollectionInfo& collectionInfo);
};

/**
 * Determines whether or not to wait for oplog visibility for a query. This is only used for
 * collection scans on the oplog.
 */
bool shouldWaitForOplogVisibility(OperationContext* opCtx,
                                  const CollectionPtr& collection,
                                  bool tailable);
}  // namespace mongo
