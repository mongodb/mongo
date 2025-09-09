/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/flow_control_ticketholder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/profile_filter.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/query/query_shape/query_shape.h"
#include "mongo/db/query/query_stats/data_bearing_node_metrics.h"
#include "mongo/db/query/query_stats/key.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/rpc/message.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"

#include <map>
#include <set>
#include <string>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <boost/optional/optional.hpp>

namespace MONGO_MOD_PUB mongo {

class CurOp;

/* lifespan is different than CurOp because of recursives with DBDirectClient */
class OpDebug {
public:
    /**
     * Holds counters for execution statistics that can be accumulated by one or more operations.
     * They're accumulated as we go for a single operation, but are also extracted and stored
     * externally if they need to be accumulated across multiple operations (which have multiple
     * CurOps), including for cursors and multi-statement transactions.
     */
    class AdditiveMetrics {
    public:
        AdditiveMetrics() = default;
        AdditiveMetrics(const AdditiveMetrics& other) {
            this->add(other);
        }

        AdditiveMetrics& operator=(const AdditiveMetrics& other) {
            reset();
            add(other);
            return *this;
        }

        /**
         * Adds all the fields of another AdditiveMetrics object together with the fields of this
         * AdditiveMetrics instance.
         */
        void add(const AdditiveMetrics& otherMetrics);

        /**
         * Adds all of the fields of the given DataBearingNodeMetrics object together with the
         * corresponding fields of this object.
         */
        void aggregateDataBearingNodeMetrics(const query_stats::DataBearingNodeMetrics& metrics);
        void aggregateDataBearingNodeMetrics(
            const boost::optional<query_stats::DataBearingNodeMetrics>& metrics);

        /**
         * Aggregate CursorMetrics (e.g., from a remote cursor) into this AdditiveMetrics instance.
         */
        void aggregateCursorMetrics(const CursorMetrics& metrics);

        /**
         * Aggregates StorageStats from the storage engine into this AdditiveMetrics instance.
         */
        void aggregateStorageStats(const StorageStats& stats);

        /**
         * Resets all members to the default state.
         */
        void reset();

        /**
         * Returns true if the AdditiveMetrics object we are comparing has the same field values as
         * this AdditiveMetrics instance.
         */
        bool equals(const AdditiveMetrics& otherMetrics) const;

        /**
         * Increments temporarilyUnavailableErrors by n.
         */
        void incrementTemporarilyUnavailableErrors(long long n);

        /**
         * Increments keysInserted by n.
         */
        void incrementKeysInserted(long long n);

        /**
         * Increments keysDeleted by n.
         */
        void incrementKeysDeleted(long long n);

        /**
         * Increments nreturned by n.
         */
        void incrementNreturned(long long n);

        /**
         * Increments nBatches by 1.
         */
        void incrementNBatches();

        /**
         * Increments ninserted by n.
         */
        void incrementNinserted(long long n);

        /**
         * Increments ndeleted by n.
         */
        void incrementNdeleted(long long n);

        /**
         * Increments nUpserted by n.
         */
        void incrementNUpserted(long long n);

        /**
         * Generates a string showing all non-empty fields. For every non-empty field field1,
         * field2, ..., with corresponding values value1, value2, ..., we will output a string in
         * the format: "<field1>:<value1> <field2>:<value2> ...".
         */
        std::string report() const;
        BSONObj reportBSON() const;

        void report(logv2::DynamicAttributes* pAttrs) const;

        boost::optional<long long> keysExamined;
        boost::optional<long long> docsExamined;
        boost::optional<long long> bytesRead;

        // Number of records that match the query.
        boost::optional<long long> nMatched;
        // Number of records returned so far.
        boost::optional<long long> nreturned;
        // Number of batches returned so far.
        boost::optional<long long> nBatches;
        // Number of records written (no no-ops).
        boost::optional<long long> nModified;
        boost::optional<long long> ninserted;
        boost::optional<long long> ndeleted;
        boost::optional<long long> nUpserted;

        // Number of index keys inserted.
        boost::optional<long long> keysInserted;
        // Number of index keys removed.
        boost::optional<long long> keysDeleted;

        // Amount of time spent executing a query.
        boost::optional<Microseconds> executionTime;

        // If query stats are being collected for this operation, stores the duration of execution
        // across the cluster. In a standalone mongod, this is just the local working time. In
        // mongod in a sharded cluster, this is the local execution time plus any execution time
        // for other nodes to do work on our behalf. In mongos, this tracks the total working time
        // across the cluster.
        boost::optional<Milliseconds> clusterWorkingTime{0};

        // Amount of time spent reading from disk in the storage engine.
        boost::optional<Microseconds> readingTime{0};

        // True if the query plan involves an in-memory sort.
        bool hasSortStage{false};
        // True if the given query used disk.
        bool usedDisk{false};
        // True if any plan(s) involved in servicing the query (including internal queries sent to
        // shards) came from the multi-planner (not from the plan cache and not a query with a
        // single solution).
        bool fromMultiPlanner{false};
        // False unless all plan(s) involved in servicing the query came from the plan cache.
        // This is because we want to report a "negative" outcome (plan cache miss) if any internal
        // query involved missed the cache. Optional because we need tri-state (true, false, not
        // set) to make the "sticky towards false" logic work.
        boost::optional<bool> fromPlanCache;

        // If query stats are being collected for this operation, stores the estimated cpu time
        // across the cluster. In a mongod, this is the local cpu time and in mongos this track the
        // total cpu time across the cluster.
        // This value will be negative if the platform does not support collecting cpu time, since
        // collecting cpu time is only supported Linux systems. If the value is greater than 0, the
        // cpu time reported is the cpu time collected.
        boost::optional<Nanoseconds> cpuNanos{0};

        // If query stats are being collected for this operation, stores the delinquency information
        // across the cluster. It's only collected in mongod shard and aggregated of all shard in
        // mongos.
        boost::optional<uint64_t> delinquentAcquisitions;
        boost::optional<Milliseconds> totalAcquisitionDelinquency;
        boost::optional<Milliseconds> maxAcquisitionDelinquency;

        boost::optional<uint64_t> numInterruptChecks;
        boost::optional<Milliseconds> overdueInterruptApproxMax;
    };

    OpDebug() = default;

    /**
     * Adds information about the current operation to "pAttrs". Since this information will end up
     * in logs, typically as part of "Slow Query" logging, this method also handles redaction and
     * removal of sensitive fields from any command BSON.
     *
     * Generally, the metrics/fields reported here should be a subset of what is reported in
     * append(). The profiler is meant to be more verbose than the slow query log.
     */
    void report(OperationContext* opCtx,
                const SingleThreadedLockStats* lockStats,
                const SingleThreadedStorageMetrics& storageMetrics,
                long long prepareReadConflicts,
                logv2::DynamicAttributes* pAttrs) const;

    void reportStorageStats(logv2::DynamicAttributes* pAttrs) const;

    /**
     * Appends information about the current operation to "builder". This info typically ends up in
     * the DB's profile collection. It is not ready to be logged as-is; redaction and removal of
     * sensitive command fields are required first.
     *
     * @param lockStats lockStats object containing locking information about the operation
     *
     * Generally, the metrics/fields reported here should be a superset of what is reported in
     * report(). The profiler is meant to be more verbose than the slow query log.
     */
    void append(OperationContext* opCtx,
                const SingleThreadedLockStats& lockStats,
                FlowControlTicketholder::CurOp flowControlStats,
                const SingleThreadedStorageMetrics& storageMetrics,
                long long prepareReadConflicts,
                bool omitCommand,
                BSONObjBuilder& builder) const;

    static std::function<BSONObj(ProfileFilter::Args args)> appendStaged(OperationContext* opCtx,
                                                                         StringSet requestedFields,
                                                                         bool needWholeDocument);
    static void appendUserInfo(const CurOp&, BSONObjBuilder&, AuthorizationSession*);

    static void appendDelinquentInfo(OperationContext* opCtx,
                                     BSONObjBuilder&,
                                     bool reportAcquisitions = true);

    /**
     * Moves relevant plan summary metrics to this OpDebug instance.
     */
    void setPlanSummaryMetrics(PlanSummaryStats&& planSummaryStats);

    /**
     * The resulting object has zeros omitted. As is typical in this file.
     */
    static BSONObj makeFlowControlObject(FlowControlTicketholder::CurOp flowControlStats);

    /**
     * Make object from $search stats with non-populated values omitted.
     */
    BSONObj makeMongotDebugStatsObject() const;

    /**
     * Gets the type of the namespace on which the current operation operates.
     */
    std::string getCollectionType(const NamespaceString& nss) const;

    /**
     * Accumulate resolved views.
     */
    void addResolvedViews(const std::vector<NamespaceString>& namespaces,
                          const std::vector<BSONObj>& pipeline);

    /**
     * Get or append the array with resolved views' info.
     */
    BSONArray getResolvedViewsInfo() const;
    void appendResolvedViewsInfo(BSONObjBuilder& builder) const;

    /**
     * Get a snapshot of the cursor metrics suitable for inclusion in a command response.
     */
    CursorMetrics getCursorMetrics() const;

    boost::optional<query_shape::QueryShapeHash> getQueryShapeHash() const;

    /**
     * Convenience method that sets 'queryShapeHash' if 'queryShapeHash' has not been previously
     * set. Currently QueryShapeHash for a given command may be computed twice (due to view
     * resolution). By preventing new QueryShapeHash overwrites we ensure that original
     * QueryShapeHash is recorded in CurOp::OpDebug.
     */
    void setQueryShapeHashIfNotPresent(OperationContext* opCtx,
                                       const boost::optional<query_shape::QueryShapeHash>& hash);

    // -------------------

    // basic options
    // _networkOp represents the network-level op code: OP_QUERY, OP_GET_MORE, OP_MSG, etc.
    NetworkOp networkOp{opInvalid};  // only set this through setNetworkOp() to keep synced
    // _logicalOp is the logical operation type, ie 'dbQuery' regardless of whether this is an
    // OP_QUERY find, a find command using OP_QUERY, or a find command using OP_MSG.
    // Similarly, the return value will be dbGetMore for both OP_GET_MORE and getMore command.
    LogicalOp logicalOp{LogicalOp::opInvalid};  // only set this through setNetworkOp()
    bool iscommand{false};

    // detailed options
    long long cursorid{-1};
    bool exhaust{false};

    // For search using mongot.
    boost::optional<long long> mongotCursorId{boost::none};
    boost::optional<long long> msWaitingForMongot{boost::none};
    long long mongotBatchNum = 0;
    BSONObj mongotCountVal = BSONObj();
    BSONObj mongotSlowQueryLog = BSONObj();

    // Vector search statistics captured for reporting by query stats.
    struct VectorSearchMetrics {
        long limit = 0;
        double numCandidatesLimitRatio = 0.0;
    };
    boost::optional<VectorSearchMetrics> vectorSearchMetrics = boost::none;

    // The accumulated spilling statistics per stage type.
    absl::flat_hash_map<PlanSummaryStats::SpillingStage, SpillingStats> spillingStatsPerStage;
    size_t sortTotalDataSizeBytes{0};  // The amount of data we've sorted in bytes

    long long keysSorted{0};       // The number of keys that we've sorted.
    long long collectionScans{0};  // The number of collection scans during query execution.
    long long collectionScansNonTailable{0};  // The number of non-tailable collection scans.
    std::set<std::string> indexesUsed;        // The indexes used during query execution.

    // True if a replan was triggered during the execution of this operation.
    boost::optional<std::string> replanReason;

    bool cursorExhausted{
        false};  // true if the cursor has been closed at end a find/getMore operation

    bool isChangeStreamQuery{false};

    BSONObj execStats;  // Owned here.

    // The hash of the PlanCache key for the query being run. This may change depending on what
    // indexes are present.
    boost::optional<uint32_t> planCacheKey;

    // The hash of the canonical query encoding CanonicalQuery::QueryShapeString
    boost::optional<uint32_t> planCacheShapeHash;

    /* The QueryStatsInfo struct was created to bundle all the queryStats related fields of CurOp &
     * OpDebug together (SERVER-83280).
     *
     * ClusterClientCursorImpl and ClientCursor also contain _queryStatsKey and _queryStatsKeyHash
     * members but NOT other members of the struct. Variable names & accesses would be more
     * consistent across the code if ClusterClientCursorImpl and ClientCursor each also had a
     * QueryStatsInfo struct, but we considered and rejected two different potential implementations
     * of this:
     *  - Option 1:
     *    Declare a QueryStatsInfo struct in each .h file. Every struct would have key and keyHash
     *    fields, and other fields would be added only to CurOp. But it seemed confusing to have
     *    slightly different structs with the same name declared three different times.
     *  - Option 2:
     *    Create a query_stats_info.h that declares QueryStatsInfo--identical to the version defined
     *    in this file. CurOp/OpDebug, ClientCursor, and ClusterClientCursorImpl would then all
     *    have their own QueryStatsInfo instances, potentially as a unique_ptr or boost::optional. A
     *    benefit to this would be the ability to to just move the entire QueryStatsInfo struct from
     *    Op to the Cursor, instead of copying it over field by field (the current method). But:
     *      - The current code moves ownership of the key, but copies the keyHash. So, for workflows
     *        that require multiple cursors, like sharding, one cursor would own the key, but all
     *        cursors would have copies of the keyHash. The problem with trying to move around the
     *        struct in its entirety is that access to the *entire* struct would be lost on the
     *        move, meaning there's no way to retain the keyHash (that doesn't largely nullify the
     *        benefits of having the struct).
     *      - It seemed odd to have ClientCursor and ClusterClientCursorImpl using the struct but
     *        never needing other fields.
     */
    struct QueryStatsInfo {
        // Uniquely identifies one query stats entry.
        // `key` may be a nullptr during a subquery execution, but will
        // be non-null at the highest-level operation as long as query stats are
        // being collected and the query is not rate limited.
        std::unique_ptr<query_stats::Key> key;
        // A cached value of `absl::HashOf(key)`.
        // Always populated if `key` is non-null at the highest-level operation.
        boost::optional<std::size_t> keyHash;
        // True if a subquery is being run, such as if the original query has been resolved to a
        // view running an aggregation pipeline. If true, stats should not be registered at the
        // current point in execution.
        bool disableForSubqueryExecution = false;
        // True if the request was a change stream request.
        bool willNeverExhaust = false;
        // Sometimes we need to request metrics as part of a higher-level operation without
        // actually caring about the metrics for this specific operation. In those cases, we
        // use metricsRequested to indicate we should request metrics from other nodes.
        bool metricsRequested = false;
    };

    QueryStatsInfo queryStatsInfo;

    // The query framework that this operation used. Will be unknown for non query operations.
    PlanExecutor::QueryFramework queryFramework{PlanExecutor::QueryFramework::kUnknown};

    // Tracks the amount of dynamic indexed loop joins in a pushed down stage.
    int dynamicIndexedLoopJoin{0};

    // Tracks the amount of indexed loop joins in a pushed down lookup stage.
    int indexedLoopJoin{0};

    // Tracks the amount of nested loop joins in a pushed down lookup stage.
    int nestedLoopJoin{0};

    // Tracks the amount of hash lookups in a pushed down lookup stage.
    int hashLookup{0};

    // Tracks the amount of spills by hash lookup in a pushed down lookup stage.
    int hashLookupSpillToDisk{0};

    // Tracks the number of spilled bytes by hash lookup in a pushed down lookup stage. The spilled
    // storage size after compression might be different from the bytes spilled.
    long long hashLookupSpillToDiskBytes{0};

    // Details of any error (whether from an exception or a command returning failure).
    Status errInfo = Status::OK();

    // Amount of time spent planning the query. Begins after parsing and ends
    // after optimizations.
    Microseconds planningTime{0};

    // Cost computed by the cost-based optimizer.
    boost::optional<double> estimatedCost;
    // Cardinality computed by the cost-based optimizer.
    boost::optional<double> estimatedCardinality;

    // Amount of CPU time used by this thread. Will remain -1 if this platform does not support
    // this feature.
    Nanoseconds cpuTime{-1};

    int responseLength{-1};

    // Shard targeting info.
    int nShards{-1};

    // Stores the duration of time spent blocked on prepare conflicts.
    Milliseconds prepareConflictDurationMillis{0};

    // Total time spent looking up database entry in the local catalog cache, including eventual
    // refreshes.
    Milliseconds catalogCacheDatabaseLookupMillis{0};

    // Total time spent looking up collection entry in the local catalog cache, including eventual
    // refreshes.
    Milliseconds catalogCacheCollectionLookupMillis{0};

    // Total time spent looking up index entries in the local cache, including eventual refreshes.
    Milliseconds catalogCacheIndexLookupMillis{0};

    // Stores the duration of time spent waiting for the shard to refresh the database and wait for
    // the database critical section.
    Milliseconds databaseVersionRefreshMillis{0};

    // Stores the duration of time spent waiting for the shard to refresh the collection and wait
    // for the collection critical section.
    Milliseconds placementVersionRefreshMillis{0};

    // Stores the duration of time spent waiting for the specified user write concern to
    // be fulfilled.
    Milliseconds waitForWriteConcernDurationMillis{0};

    // Stores the duration of execution after removing time spent blocked.
    Milliseconds workingTimeMillis{0};

    // Stores the amount of the data processed by the throttle cursors in MB/sec.
    boost::optional<float> dataThroughputLastSecond;
    boost::optional<float> dataThroughputAverage;

    // Used to track the amount of time spent waiting for a response from remote operations.
    boost::optional<Microseconds> remoteOpWaitTime;

    // Stores the current operation's count of these metrics. If they are needed to be accumulated
    // elsewhere, they should be extracted by another aggregator (like the ClientCursor) to ensure
    // these only ever reflect just this CurOp's consumption.
    AdditiveMetrics additiveMetrics;

    // Stores storage statistics.
    std::unique_ptr<StorageStats> storageStats;

    // Stores storage statistics from the spill engine.
    std::unique_ptr<StorageStats> spillStorageStats;

    bool waitingForFlowControl{false};

    // Records the WC that was waited on during the operation. (The WC in opCtx can't be used
    // because it's only set while the Command itself executes.)
    boost::optional<WriteConcernOptions> writeConcern;

    // Whether this is an oplog getMore operation for replication oplog fetching.
    bool isReplOplogGetMore{false};

    // Maps namespace of a resolved view to its dependency chain and the fully unrolled pipeline. To
    // make log line deterministic and easier to test, use ordered map. As we don't expect many
    // resolved views per query, a hash map would unlikely provide any benefits.
    std::map<NamespaceString, std::pair<std::vector<NamespaceString>, std::vector<BSONObj>>>
        resolvedViews;

private:
    // The hash of query_shape::QueryShapeHash.
    boost::optional<query_shape::QueryShapeHash> _queryShapeHash;
};
}  // namespace MONGO_MOD_PUB mongo
