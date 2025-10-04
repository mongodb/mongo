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

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/dbclient_base.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/collection_index_usage_tracker.h"
#include "mongo/db/database_name.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/exec_shard_filter_policy.h"
#include "mongo/db/local_catalog/collection_type.h"
#include "mongo/db/local_catalog/shard_role_api/resource_yielder.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/sharded_agg_helpers_targeting_policy.h"
#include "mongo/db/pipeline/storage_stats_spec_gen.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/client_cursor/generic_cursor.h"
#include "mongo/db/query/client_cursor/generic_cursor_gen.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/write_ops/write_ops_exec.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/storage/backup_cursor_hooks.h"
#include "mongo/db/storage/backup_cursor_state.h"
#include "mongo/db/storage/key_format.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/spill_table.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/task_executor.h"
#include "mongo/util/uuid.h"

#include <cstdint>
#include <deque>
#include <list>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <boost/intrusive_ptr.hpp>
#include <boost/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

class ShardFilterer;
class ExpressionContext;
class JsExecution;

class Pipeline;
class RoutingContext;
// TODO SERVER-110774 investigate removing 'CollectionRoutingInfo' and
// 'CollectionOrViewAcquisition' forward declarations.
class CollectionRoutingInfo;
class CollectionOrViewAcquisition;
class TransactionHistoryIteratorBase;

/**
 * Any functionality needed by an aggregation stage that is either context specific to a mongod or
 * mongos process, or is only compiled in to one of those two binaries must be accessed via this
 * interface. This allows all DocumentSources to be parsed on either mongos or mongod, but only
 * executable where it makes sense.
 */
class MongoProcessInterface {
public:
    /**
     * Storage for a batch of BSON Objects to be updated in the write namespace. For each element
     * in the batch we store a tuple of the following elements:
     *   1. BSONObj - specifies the query that identifies a document in the to collection to be
     *      updated.
     *   2. write_ops::UpdateModification - either the new document we want to upsert or insert into
     *      the collection (i.e. a 'classic' replacement update), or the pipeline to run to compute
     *      the new document.
     *   3. boost::optional<BSONObj> - for pipeline-style updates, specifies variables that can be
     *      referred to in the pipeline performing the custom update.
     */
    using BatchObject =
        std::tuple<BSONObj, write_ops::UpdateModification, boost::optional<BSONObj>>;
    using BatchedObjects = std::vector<BatchObject>;
    using CollectionMetadata =
        std::variant<std::monostate,
                     std::reference_wrapper<const CollectionOrViewAcquisition>,
                     std::reference_wrapper<const CollectionRoutingInfo>>;


    enum class UpsertType {
        kNone,              // This operation is not an upsert.
        kGenerateNewDoc,    // If no documents match, generate a new document using the update spec.
        kInsertSuppliedDoc  // If no documents match, insert the document supplied in 'c.new' as-is.
    };

    enum class CurrentOpConnectionsMode { kIncludeIdle, kExcludeIdle };
    enum class CurrentOpUserMode { kIncludeAll, kExcludeOthers };
    enum class CurrentOpTruncateMode { kNoTruncation, kTruncateOps };
    enum class CurrentOpLocalOpsMode { kLocalMongosOps, kRemoteShardOps };
    enum class CurrentOpSessionsMode { kIncludeIdle, kExcludeIdle };
    enum class CurrentOpCursorMode { kIncludeCursors, kExcludeCursors };

    /**
     * Interface which estimates the size of a given write operation.
     */
    class WriteSizeEstimator {
    public:
        virtual ~WriteSizeEstimator() = default;

        /**
         * Set of functions which estimate the entire size of a write command except for the array
         * of write statements themselves.
         */
        virtual int estimateInsertHeaderSize(
            const write_ops::InsertCommandRequest& insertReq) const = 0;
        virtual int estimateUpdateHeaderSize(
            const write_ops::UpdateCommandRequest& updateReq) const = 0;

        /**
         * Set of functions which estimate the size of a single write statement.
         */
        virtual int estimateInsertSizeBytes(const BSONObj& insert) const = 0;
        virtual int estimateUpdateSizeBytes(const BatchObject& batchObject,
                                            UpsertType type) const = 0;
    };

    /**
     * Factory function to create MongoProcessInterface of the right type. The implementation will
     * be installed by a lib higher up in the link graph depending on the application type.
     */
    static std::shared_ptr<MongoProcessInterface> create(OperationContext* opCtx);

    /**
     * This structure holds the result of a batched update operation, such as the number of
     * documents that matched the query predicate, and the number of documents modified by the
     * update operation.
     */
    struct UpdateResult {
        int64_t nMatched{0};
        int64_t nModified{0};
    };

    MongoProcessInterface(std::shared_ptr<executor::TaskExecutor> executor)
        : taskExecutor(std::move(executor)) {}

    virtual ~MongoProcessInterface() {};

    /**
     * Returns an instance of a 'WriteSizeEstimator' interface.
     */
    virtual std::unique_ptr<WriteSizeEstimator> getWriteSizeEstimator(
        OperationContext* opCtx, const NamespaceString& ns) const = 0;

    /**
     * Creates a new TransactionHistoryIterator object. Only applicable in processes which support
     * locally traversing the oplog.
     */
    virtual std::unique_ptr<TransactionHistoryIteratorBase> createTransactionHistoryIterator(
        repl::OpTime time) const = 0;

    /**
     * Note that in some rare cases this could return a false negative but will never return a false
     * positive. This method will be fixed in the future once it becomes possible to avoid false
     * negatives. Caller should always attach shardVersion when sending request against nss based
     * on this information.
     *
     * DO NOT use this function to make a decision that affects query correctness.
     */
    virtual bool isSharded(OperationContext* opCtx, const NamespaceString& ns) = 0;

    /**
     * TODO SERVER-79508 validate callers of this function remain correct.
     *
     * Returns false if the current request only handles parsing and validating queries. In other
     * words, we are not executing queries. Examples include query analysis for queryable
     * encryption, executing pipeline-style operations in the Update system, and creating a Query
     * Shape. This function only returns false when the process interface is of type
     * 'StubMongoProcessInterface'.
     *
     */
    virtual bool isExpectedToExecuteQueries() {
        return true;
    }

    /**
     * Utility which determines which shard we should merge on. More precisely, if 'nss' is
     * unsplittable or untracked, we should route to the shard which owns 'nss'. Note that this
     * decision is inherently racy and subject to become stale. This is okay because either choice
     * will work correctly, we are simply applying a heuristic optimization.
     */
    virtual boost::optional<ShardId> determineSpecificMergeShard(
        OperationContext* opCtx, const NamespaceString& ns) const = 0;

    /**
     * Advances the proxied write time associated with the client in ReplClientInfo to
     * be at least as high as the one tracked by the OperationTimeTracker associated with the
     * given operation context.
     */
    virtual void updateClientOperationTime(OperationContext* opCtx) const = 0;

    /**
     * Executes 'insertCommand' against 'ns' and returns an error Status if the insert fails. If
     * 'targetEpoch' is set, throws ErrorCodes::StaleEpoch if the targeted collection does not have
     * the same epoch or the epoch changes during the course of the insert.
     */
    virtual Status insert(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                          const NamespaceString& ns,
                          std::unique_ptr<write_ops::InsertCommandRequest> insertCommand,
                          const WriteConcernOptions& wc,
                          boost::optional<OID> targetEpoch) = 0;

    virtual Status insertTimeseries(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                    const NamespaceString& ns,
                                    std::unique_ptr<write_ops::InsertCommandRequest> insertCommand,
                                    const WriteConcernOptions& wc,
                                    boost::optional<OID> targetEpoch) = 0;
    /**
     * Executes the updates described by 'updateCommand'. Returns an error Status if any of the
     * updates fail, otherwise returns an 'UpdateResult' objects with the details of the update
     * operation.  If 'targetEpoch' is set, throws ErrorCodes::StaleEpoch if the targeted collection
     * does not have the same epoch, or if the epoch changes during the update.
     */
    virtual StatusWith<UpdateResult> update(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const NamespaceString& ns,
        std::unique_ptr<write_ops::UpdateCommandRequest> updateCommand,
        const WriteConcernOptions& wc,
        UpsertType upsert,
        bool multi,
        boost::optional<OID> targetEpoch) = 0;

    /**
     * Returns index usage statistics for each index on collection 'ns' along with additional
     * information including the index specification and whether the index is currently being built.
     *
     * By passing true for 'addShardName', the caller can request that each document in the
     * resulting vector includes a 'shard' field which denotes this node's shard name. It is illegal
     * to set this option unless this node is a shardsvr.
     */
    virtual std::vector<Document> getIndexStats(OperationContext* opCtx,
                                                const NamespaceString& ns,
                                                StringData host,
                                                bool addShardName) = 0;

    virtual std::vector<BSONObj> getIndexSpecs(OperationContext* opCtx,
                                               const NamespaceString& ns,
                                               bool includeBuildUUIDs) = 0;

    /**
     * Returns all documents in `_mdb_catalog`.
     */
    virtual std::deque<BSONObj> listCatalog(OperationContext* opCtx) const = 0;

    /**
     * Returns the catalog entry for the given namespace, if it exists.
     */
    virtual boost::optional<BSONObj> getCatalogEntry(
        OperationContext* opCtx,
        const NamespaceString& ns,
        const boost::optional<UUID>& collUUID = boost::none) const = 0;

    /**
     * Appends operation latency statistics for collection "nss" to "builder"
     */
    virtual void appendLatencyStats(OperationContext* opCtx,
                                    const NamespaceString& nss,
                                    bool includeHistograms,
                                    BSONObjBuilder* builder) const = 0;

    /**
     * Appends storage statistics for collection "nss" to "builder".
     *
     * By passing a BSONObj as the parameter 'filterObj' in this function, the caller can request
     * specific stats to be appended to parameter 'builder'. By passing 'boost::none' to
     * 'filterObj', the caller is requesting to append all possible storage stats.
     */
    virtual Status appendStorageStats(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                      const NamespaceString& nss,
                                      const StorageStatsSpec& spec,
                                      BSONObjBuilder* builder,
                                      const boost::optional<BSONObj>& filterObj) const = 0;

    /**
     * Appends the record count for collection "nss" to "builder".
     */
    virtual Status appendRecordCount(OperationContext* opCtx,
                                     const NamespaceString& nss,
                                     BSONObjBuilder* builder) const = 0;

    /**
     * Appends the exec stats for the collection 'nss' to 'builder'.
     */
    virtual Status appendQueryExecStats(OperationContext* opCtx,
                                        const NamespaceString& nss,
                                        BSONObjBuilder* builder) const = 0;

    /**
     * Appends the operation stats for "nss" to "builder".
     */
    virtual void appendOperationStats(OperationContext* opCtx,
                                      const NamespaceString& nss,
                                      BSONObjBuilder* builder) const = 0;
    /**
     * Gets the collection options for the collection given by 'nss'. If the nss represents a view,
     * we will return the user spec that was used to create the view. Future callers may want to
     * parameterize this behavior.
     */
    virtual BSONObj getCollectionOptions(OperationContext* opCtx, const NamespaceString& nss) = 0;

    virtual UUID fetchCollectionUUIDFromPrimary(OperationContext* opCtx,
                                                const NamespaceString& nss) = 0;

    /**
     * Returns the query shape collection type in the namespace given by 'nss'. This function holds
     * an acquisition that is freed right after execution, and can't guarantee if the collection or
     * view exists in the future.
     */
    virtual query_shape::CollectionType getCollectionType(OperationContext* opCtx,
                                                          const NamespaceString& nss) = 0;

    /**
     * Performs the given rename command if the collection given by 'targetNs' has the same options
     * as specified in 'originalCollectionOptions', and has the same indexes as 'originalIndexes'.
     *
     * Throws an exception if the collection options and/or indexes are different.
     */
    virtual void renameIfOptionsAndIndexesHaveNotChanged(
        OperationContext* opCtx,
        const NamespaceString& sourceNs,
        const NamespaceString& targetNs,
        bool dropTarget,
        bool stayTemp,
        const BSONObj& originalCollectionOptions,
        const std::vector<BSONObj>& originalIndexes) = 0;

    /**
     * Creates a collection on the given database by running the given command. On shardsvr targets
     * the primary shard of 'dbName'.
     */
    virtual void createCollection(OperationContext* opCtx,
                                  const DatabaseName& dbName,
                                  const BSONObj& cmdObj) = 0;

    /**
     * Creates a temporary collection 'nss', with the given 'collectionOptions'.
     *
     * On non-shard servers, temporary collections are collections with the 'temp' property set to
     * true. On shard servers, temporary collections don't set the 'temp' property to true; instead,
     * they are annotated on the 'kAggTempCollections' collection on this shard. In both cases,
     * temporary collections are dropped (garbage-collected) on stepup (or startup in the case of
     * standalone nodes).
     *
     * This function accepts an optional 'dataShard' parameter that indicates the shard where the
     * temporary collection should be created.
     */
    virtual void createTempCollection(OperationContext* opCtx,
                                      const NamespaceString& nss,
                                      const BSONObj& collectionOptions,
                                      boost::optional<ShardId> dataShard = boost::none) = 0;
    /**
     * Creates the view backing a time-series collection.
     */
    virtual void createTimeseriesView(OperationContext* opCtx,
                                      const NamespaceString& ns,
                                      const BSONObj& cmdObj,
                                      const TimeseriesOptions& userOpts) = 0;

    /**
     * Runs createIndexes on the given database for the given index specs. If running on a shardsvr
     * this targets the primary shard of the database part of 'ns'.
     */
    virtual void createIndexesOnEmptyCollection(OperationContext* opCtx,
                                                const NamespaceString& ns,
                                                const std::vector<BSONObj>& indexSpecs) = 0;

    virtual void dropCollection(OperationContext* opCtx, const NamespaceString& collection) = 0;

    /**
     * Drops a temporary collection that was previously created using 'createTempCollection'.
     */
    virtual void dropTempCollection(OperationContext* opCtx, const NamespaceString& nss) = 0;

    /**
     * Accepts a pipeline and returns a new one which will draw input from the underlying
     * collection. Behavior for how to finalize the pipeline, such as optimizations and
     * translations, before a cursor is attached must be defined inside 'finalizePipeline'. To
     * attach a cursor this function calls 'preparePipelineForExecution' (see below).
     *
     * This function guarantees that optimizing, translating and preparing the pipeline for
     * execution will use a single snapshot of collection metadata ('CollectionOrViewAcquisition' or
     * 'RoutingContext').
     */
    virtual std::unique_ptr<Pipeline> finalizeAndMaybePreparePipelineForExecution(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        Pipeline* ownedPipeline,
        bool attachCursorAfterOptimizing,
        std::function<void(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                           Pipeline* pipeline,
                           CollectionMetadata collData)> finalizePipeline = nullptr,
        ShardTargetingPolicy shardTargetingPolicy = ShardTargetingPolicy::kAllowed,
        boost::optional<BSONObj> readConcern = boost::none,
        bool shouldUseCollectionDefaultCollator = false) = 0;

    /**
     * Accepts a pipeline and returns a new one which will draw input from the underlying
     * collection. Performs no further optimization of the pipeline. NamespaceNotFound will be
     * thrown if ExpressionContext has a UUID and that UUID doesn't exist anymore. That should be
     * the only case where NamespaceNotFound is returned.
     *
     * This function takes ownership of the 'pipeline' argument as if it were a unique_ptr.
     * Changing it to a unique_ptr introduces a circular dependency on certain platforms where the
     * compiler expects to find an implementation of PipelineDeleter.
     *
     * If `shardTargetingPolicy` is kNotAllowed, the cursor will only be for local reads regardless
     * of whether or not this function is called in a sharded environment.
     */
    virtual std::unique_ptr<Pipeline> preparePipelineForExecution(
        Pipeline* pipeline,
        ShardTargetingPolicy shardTargetingPolicy = ShardTargetingPolicy::kAllowed,
        boost::optional<BSONObj> readConcern = boost::none) = 0;

    /**
     * Prepares a pipeline for execution based on the provided aggregation request and pipeline.
     * This method preserves any aggregation options specified in the AggregateCommandRequest.
     *
     * The 'shardCursorsSortSpec' parameter, if provided, specifies the expected sort order of
     * cursors from the shards. The cursors must be sorted according to this specification and
     * include a "$sortKey" metadata field for result comparison.
     *
     * The routing of the corresponding 'aggRequest' to local or remote shards is determined by the
     * 'shardTargetingPolicy'. If 'shardTargetingPolicy' is set to kNotAllowed, the request will
     * only be routed for local reads, regardless of the sharded environment.
     *
     * The 'readConcern' parameter specifies the read concern level for the aggregation request. If
     * 'shouldUseCollectionDefaultCollator' is set to true, the default collection collator will be
     * attached to the 'expCtx'.
     *
     * This function takes ownership of the 'pipeline' argument, which is expected to be a valid
     * pointer to a Pipeline object.
     */
    virtual std::unique_ptr<Pipeline> preparePipelineForExecution(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const AggregateCommandRequest& aggRequest,
        Pipeline* pipeline,
        boost::optional<BSONObj> shardCursorsSortSpec = boost::none,
        ShardTargetingPolicy shardTargetingPolicy = ShardTargetingPolicy::kAllowed,
        boost::optional<BSONObj> readConcern = boost::none,
        bool shouldUseCollectionDefaultCollator = false) = 0;

    /**
     * Accepts a pipeline and attaches a cursor source to it. Returns a BSONObj of the form
     * {"pipeline": <explainOutput>}. Note that <explainOutput> can be an object (shardsvr) or an
     * array (non_shardsvr).
     */
    virtual BSONObj preparePipelineAndExplain(Pipeline* ownedPipeline,
                                              ExplainOptions::Verbosity verbosity) = 0;

    /**
     * Accepts a pipeline and returns a new one which will draw input from the underlying
     * collection _locally_. Trying to run this method on mongos is a programming error. Running
     * this method on a shard server will only return results which match the pipeline on that
     * shard.

     * Performs no further optimization of the pipeline. CollectionUUIDMismatch exception will be
     * thrown if 'aggRequest' has 'collectionUUID' set and it doesn't match with the UUID of the
     * collection acquired via the nss.
     *
     * When 'shouldUseCollectionDefaultCollator' is set to true, the collator of the target
     * collection will be used instead of previously provided collator.
     *
     * This function takes ownership of the 'pipeline' argument as if it were a unique_ptr.
     * Changing it to a unique_ptr introduces a circular dependency on certain platforms where the
     * compiler expects to find an implementation of PipelineDeleter.
     */
    virtual std::unique_ptr<Pipeline> attachCursorSourceToPipelineForLocalRead(
        Pipeline* pipeline,
        boost::optional<const AggregateCommandRequest&> aggRequest = boost::none,
        bool shouldUseCollectionDefaultCollator = false,
        ExecShardFilterPolicy shardFilterPolicy = AutomaticShardFiltering{}) = 0;

    /**
     * Accepts a pipeline and returns a new one which will draw input from the underlying collection
     * _locally_. Behavior for how to finalize the pipeline, such as optimizations and translations,
     * before a cursor is attached must be defined inside 'finalizePipeline'. To attach a cursor
     * this function calls 'attachCursorSourceToPipelineForLocalRead' (see below).
     *
     * This function guarantees that parsing and preparing the pipeline for execution will use a
     * single snapshot of collection metadata ('CollectionOrViewAcquisition').
     */
    virtual std::unique_ptr<Pipeline> finalizeAndAttachCursorToPipelineForLocalRead(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        Pipeline* ownedPipeline,
        bool attachCursorAfterOptimizing,
        std::function<void(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                           Pipeline* pipeline,
                           CollectionMetadata collData)> finalizePipeline = nullptr,
        bool shouldUseCollectionDefaultCollator = false,
        boost::optional<const AggregateCommandRequest&> aggRequest = boost::none,
        ExecShardFilterPolicy shardFilterPolicy = AutomaticShardFiltering{}) = 0;

    /**
     * Returns a vector of owned BSONObjs, each of which contains details of an in-progress
     * operation or, optionally, an idle connection. If userMode is kIncludeAllUsers, report
     * operations for all authenticated users; otherwise, report only the current user's operations.
     */
    virtual std::vector<BSONObj> getCurrentOps(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        CurrentOpConnectionsMode connMode,
        CurrentOpSessionsMode sessionMode,
        CurrentOpUserMode userMode,
        CurrentOpTruncateMode,
        CurrentOpCursorMode) const = 0;

    /**
     * Returns the name of the local shard if sharding is enabled, or an empty string.
     */
    virtual std::string getShardName(OperationContext* opCtx) const = 0;

    /**
     * Returns the the local shard if this process is a shardsvr, else boost::none.
     */
    virtual boost::optional<ShardId> getShardId(OperationContext* opCtx) const = 0;

    /**
     * Returns whether or not this process is running as part of a sharded cluster.
     */
    virtual bool inShardedEnvironment(OperationContext* opCtx) const = 0;

    /**
     * Returns the "host:port" string for this node.
     */
    virtual std::string getHostAndPort(OperationContext* opCtx) const = 0;

    /**
     * Returns the fields of the document key (in order) for the collection 'nss', according to the
     * CatalogCache. The document key fields are the shard key (if sharded) and the _id (if not
     * already in the shard key). If _id is not in the shard key, it is added last. If the
     * collection is not sharded or is not known to exist, returns only _id. Does not refresh the
     * CatalogCache.
     */
    virtual std::vector<FieldPath> collectDocumentKeyFieldsActingAsRouter(
        OperationContext* opCtx,
        const NamespaceString&,
        RoutingContext* routingCtx = nullptr) const = 0;

    /**
     * Returns zero or one documents with the document key 'documentKey'. 'documentKey' is treated
     * as a unique identifier of a document, and may include an _id or all fields from the shard key
     * and an _id. The lookup is performed using collection's default collator.
     *
     * The method will validate that the requested collection's UUID matches the provided
     * 'collectionUUID' if specified. In case of collection not being present or UUID mismatch,
     * boost::none will be returned.
     *
     * The method ensures that only one document is returned for the specified 'documentKey'. If
     * more than one document is found, TooManyMatchingDocuments exception will be thrown.
     *
     * If this interface needs to send requests (possibly to other nodes) in order to look up the
     * document, 'readConcern' will be attached to these requests. Otherwise 'readConcern' will be
     * ignored.
     */
    virtual boost::optional<Document> lookupSingleDocument(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const NamespaceString& nss,
        boost::optional<UUID> collectionUUID,
        const Document& documentKey,
        boost::optional<BSONObj> readConcern) = 0;

    /**
     * Returns zero or one document with the document _id being equal to 'documentKey'. The document
     * is looked up only on the current node. Returns boost::none if no matching documents were
     * found, including cases where the given namespace does not exist. It is illegal to call this
     * method on nodes other than mongod.
     */
    virtual boost::optional<Document> lookupSingleDocumentLocally(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const NamespaceString& nss,
        const Document& documentKey) = 0;

    /**
     * Returns a vector of all idle (non-pinned) local cursors.
     */
    virtual std::vector<GenericCursor> getIdleCursors(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        CurrentOpUserMode userMode) const = 0;

    /**
     * The following methods forward to the BackupCursorHooks decorating the ServiceContext.
     */
    virtual BackupCursorState openBackupCursor(OperationContext* opCtx,
                                               const StorageEngine::BackupOptions& options) = 0;

    virtual void closeBackupCursor(OperationContext* opCtx, const UUID& backupId) = 0;

    virtual BackupCursorExtendState extendBackupCursor(OperationContext* opCtx,
                                                       const UUID& backupId,
                                                       const Timestamp& extendTo) = 0;

    /**
     * Returns a vector of BSON objects, where each entry in the vector describes a plan cache entry
     * inside the cache for the given namespace. Only those entries which match the supplied
     * MatchExpression are returned.
     */
    virtual std::vector<BSONObj> getMatchingPlanCacheEntryStats(OperationContext*,
                                                                const NamespaceString&,
                                                                const MatchExpression*) const = 0;

    enum class SupportingUniqueIndex {
        None,        // No unique index
        NotNullish,  // Sparse unique index, so only not nullish values are guaranteed to be unique
        Full,        // Regular unique index, so any combination of indexed values is unique
    };
    /**
     * Checks, if there is an index on 'nss' with properties that will guarantee that a
     * document with non-array values for each of 'fieldPaths' will have at most one matching
     * document in 'nss'.
     *
     * If such index doesn't exist, returns None. If such index is sparse, returns NotNullish,
     * because only not nullish values are guaranteed to be unique. Otherwise, returns Full.
     *
     * Specifically, such an index must include all the fields, be unique, not be a partial index,
     * and match the operation's collation as given by 'expCtx'.
     */
    virtual SupportingUniqueIndex fieldsHaveSupportingUniqueIndex(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const NamespaceString& nss,
        const std::set<FieldPath>& fieldPaths) const = 0;

    /**
     * Consults the CatalogCache to determine if this node has routing information for the
     * collection given by 'nss' which reports the same epoch as given by
     * 'targetCollectionPlacementVersion'. Major and minor versions in
     * 'targetCollectionPlacementVersion' are ignored.
     */
    virtual void checkRoutingInfoEpochOrThrow(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const NamespaceString& nss,
        ChunkVersion targetCollectionPlacementVersion) const = 0;

    /**
     * Get all the databases from the server.
     */
    virtual std::vector<DatabaseName> getAllDatabases(OperationContext* opCtx,
                                                      boost::optional<TenantId> tenantId) = 0;

    /**
     * Run listCollections command for the given database.
     */
    virtual std::vector<BSONObj> runListCollections(OperationContext* opCtx,
                                                    const DatabaseName& db,
                                                    bool addPrimaryShard = false) = 0;

    /**
     * Used to enforce the constraint that the foreign collection must be unsharded.
     */
    class ScopedExpectUnshardedCollection {
    public:
        virtual ~ScopedExpectUnshardedCollection() = default;
    };
    virtual std::unique_ptr<ScopedExpectUnshardedCollection> expectUnshardedCollectionInScope(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const boost::optional<DatabaseVersion>& dbVersion) = 0;

    using DocumentKeyResolutionMetadata =
        std::tuple<std::set<FieldPath>, boost::optional<ChunkVersion>, SupportingUniqueIndex>;
    /**
     * If the user did not provide the 'fieldPaths' set, a default unique key will be picked,
     * which can be either the "_id" field, or a shard key, depending on the 'outputNs' collection
     * type and the server type (mongod or mongos). Also returns an optional ChunkVersion,
     * populated with the version stored in the sharding catalog when we asked for the shard key
     * (on mongos only). On mongod, this is the value of the 'targetCollectionPlacementVersion'
     * parameter, which is the target placement version of the collection, as sent by mongos.
     */
    virtual DocumentKeyResolutionMetadata ensureFieldsUniqueOrResolveDocumentKey(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        boost::optional<std::set<FieldPath>> fieldPaths,
        boost::optional<ChunkVersion> targetCollectionPlacementVersion,
        const NamespaceString& outputNs) const = 0;

    std::shared_ptr<executor::TaskExecutor> taskExecutor;

    /**
     * Creates a spill table.
     */
    virtual std::unique_ptr<SpillTable> createSpillTable(
        const boost::intrusive_ptr<ExpressionContext>& expCtx, KeyFormat keyFormat) const = 0;

    /**
     * Writes the records in 'records' to the spill table. Asserts that the writes succeeded.
     */
    virtual void writeRecordsToSpillTable(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                          SpillTable& spillTable,
                                          std::vector<Record>* records) const = 0;

    /**
     * Searches for the RecordId 'rID' in 'spillTable'. Asserts that a document was found.
     */
    virtual Document readRecordFromSpillTable(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                              const SpillTable& spillTable,
                                              RecordId rID) const = 0;

    /**
     * Checks if the RecordId 'rID' is present in 'spillTable'.
     */
    virtual bool checkRecordInSpillTable(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                         const SpillTable& spillTable,
                                         RecordId rID) const = 0;

    /**
     * Deletes the record with RecordId `rID` from `spillTable`.
     */
    virtual void deleteRecordFromSpillTable(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                            SpillTable& spillTable,
                                            RecordId rID) const = 0;

    /**
     * Deletes all records from `spillTable`.
     */
    virtual void truncateSpillTable(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                    SpillTable& spillTable) const = 0;
};

}  // namespace mongo
