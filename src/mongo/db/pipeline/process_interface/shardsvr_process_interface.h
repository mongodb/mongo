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

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <list>
#include <memory>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/oid.h"
#include "mongo/db/database_name.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/process_interface/common_mongod_process_interface.h"
#include "mongo/db/pipeline/process_interface/common_process_interface.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/pipeline/sharded_agg_helpers_targeting_policy.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/database_version.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

namespace mongo {

/**
 * Specialized version of the MongoDInterface when this node is a shard server.
 */
class ShardServerProcessInterface final : public CommonMongodProcessInterface {
public:
    using CommonMongodProcessInterface::CommonMongodProcessInterface;

    /**
     * Note: Cannot be called while holding a lock. Refreshes from the config servers if the
     * metadata for the given namespace does not exist. Otherwise, will not automatically refresh,
     * so the answer may be stale or become stale after calling. Caller should always attach
     * shardVersion when sending request against nss based on this information.
     */
    bool isSharded(OperationContext* opCtx, const NamespaceString& nss) final;

    void checkRoutingInfoEpochOrThrow(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                      const NamespaceString& nss,
                                      ChunkVersion targetCollectionPlacementVersion) const final;

    std::unique_ptr<WriteSizeEstimator> getWriteSizeEstimator(
        OperationContext* opCtx, const NamespaceString& ns) const final {
        return std::make_unique<TargetPrimaryWriteSizeEstimator>();
    }

    std::vector<FieldPath> collectDocumentKeyFieldsActingAsRouter(
        OperationContext*, const NamespaceString&) const final {
        // We don't expect anyone to use this method on the shard itself (yet). This is currently
        // only used for $merge. For $out in a sharded cluster, the mongos is responsible for
        // collecting the document key fields before serializing them and sending them to the
        // shards. This is logically a MONGO_UNREACHABLE, but a malicious user could construct a
        // request to send directly to the shards which does not include the uniqueKey, so we must
        // be prepared to gracefully error.
        uasserted(50997, "Unexpected attempt to consult catalog cache on a shard server");
    }

    boost::optional<Document> lookupSingleDocument(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const NamespaceString& nss,
        UUID collectionUUID,
        const Document& documentKey,
        boost::optional<BSONObj> readConcern) final;

    Status insert(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                  const NamespaceString& ns,
                  std::unique_ptr<write_ops::InsertCommandRequest> insertCommand,
                  const WriteConcernOptions& wc,
                  boost::optional<OID> targetEpoch) final;

    StatusWith<UpdateResult> update(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                    const NamespaceString& ns,
                                    std::unique_ptr<write_ops::UpdateCommandRequest> updateCommand,
                                    const WriteConcernOptions& wc,
                                    UpsertType upsert,
                                    bool multi,
                                    boost::optional<OID> targetEpoch) final;

    BSONObj preparePipelineAndExplain(Pipeline* ownedPipeline,
                                      ExplainOptions::Verbosity verbosity) final;

    BSONObj getCollectionOptions(OperationContext* opCtx, const NamespaceString& nss) final;

    std::list<BSONObj> getIndexSpecs(OperationContext* opCtx,
                                     const NamespaceString& ns,
                                     bool includeBuildUUIDs) final;
    void renameIfOptionsAndIndexesHaveNotChanged(OperationContext* opCtx,
                                                 const NamespaceString& sourceNs,
                                                 const NamespaceString& targetNs,
                                                 bool dropTarget,
                                                 bool stayTemp,
                                                 const BSONObj& originalCollectionOptions,
                                                 const std::list<BSONObj>& originalIndexes) final;
    void createCollection(OperationContext* opCtx,
                          const DatabaseName& dbName,
                          const BSONObj& cmdObj) final;
    void createTempCollection(OperationContext* opCtx,
                              const NamespaceString& nss,
                              const BSONObj& collectionOptions) final;
    void createIndexesOnEmptyCollection(OperationContext* opCtx,
                                        const NamespaceString& ns,
                                        const std::vector<BSONObj>& indexSpecs) final;
    void dropCollection(OperationContext* opCtx, const NamespaceString& collection) final;

    void dropTempCollection(OperationContext* opCtx, const NamespaceString& nss) final;

    /**
     * If 'allowTargetingShards' is true, splits the pipeline and dispatch half to the shards,
     * leaving the merging half executing in this process after attaching a $mergeCursors. Will
     * retry on network errors and also on StaleConfig errors to avoid restarting the entire
     * operation.
     */
    std::unique_ptr<Pipeline, PipelineDeleter> attachCursorSourceToPipeline(
        Pipeline* pipeline,
        ShardTargetingPolicy shardTargetingPolicy = ShardTargetingPolicy::kAllowed,
        boost::optional<BSONObj> readConcern = boost::none) final;

    std::unique_ptr<Pipeline, PipelineDeleter> attachCursorSourceToPipeline(
        const AggregateCommandRequest& aggRequest,
        Pipeline* pipeline,
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        boost::optional<BSONObj> shardCursorsSortSpec = boost::none,
        ShardTargetingPolicy shardTargetingPolicy = ShardTargetingPolicy::kAllowed,
        boost::optional<BSONObj> readConcern = boost::none) final;

    std::unique_ptr<ScopedExpectUnshardedCollection> expectUnshardedCollectionInScope(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const boost::optional<DatabaseVersion>& dbVersion) override;

    void createTimeseriesView(OperationContext* opCtx,
                              const NamespaceString& ns,
                              const BSONObj& cmdObj,
                              const TimeseriesOptions& userOpts) final;

    Status insertTimeseries(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                            const NamespaceString& ns,
                            std::unique_ptr<write_ops::InsertCommandRequest> insertCommand,
                            const WriteConcernOptions& wc,
                            boost::optional<OID> targetEpoch) final;
};

}  // namespace mongo
