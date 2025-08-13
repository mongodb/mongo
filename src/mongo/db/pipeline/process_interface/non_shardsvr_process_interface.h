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
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/oid.h"
#include "mongo/db/client.h"
#include "mongo/db/database_name.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/shard_filterer.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/process_interface/common_mongod_process_interface.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/pipeline/sharded_agg_helpers_targeting_policy.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/task_executor.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

#include <list>
#include <memory>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

/**
 * Class to provide access to mongod-specific implementations of methods required by some
 * document sources.
 */
class NonShardServerProcessInterface : public CommonMongodProcessInterface {
public:
    bool isSharded(OperationContext* opCtx, const NamespaceString& nss) final {
        return false;
    }

    std::list<BSONObj> getIndexSpecs(OperationContext* opCtx,
                                     const NamespaceString& ns,
                                     bool includeBuildUUIDs) override;

    std::unique_ptr<Pipeline> preparePipelineForExecution(
        Pipeline* pipeline,
        ShardTargetingPolicy shardTargetingPolicy = ShardTargetingPolicy::kAllowed,
        boost::optional<BSONObj> readConcern = boost::none) override;

    std::unique_ptr<Pipeline> preparePipelineForExecution(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const AggregateCommandRequest& aggRequest,
        Pipeline* pipeline,
        boost::optional<BSONObj> shardCursorsSortSpec = boost::none,
        ShardTargetingPolicy shardTargetingPolicy = ShardTargetingPolicy::kAllowed,
        boost::optional<BSONObj> readConcern = boost::none,
        bool shouldUseCollectionDefaultCollator = false) override;

    BSONObj preparePipelineAndExplain(Pipeline* ownedPipeline,
                                      ExplainOptions::Verbosity verbosity) override;

    void checkRoutingInfoEpochOrThrow(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const NamespaceString& nss,
        ChunkVersion targetCollectionPlacementVersion) const override {
        uasserted(51020, "unexpected request to consult sharding catalog on non-shardsvr");
    }

    std::vector<DatabaseName> getAllDatabases(OperationContext* opCtx,
                                              boost::optional<TenantId> tenantId) final;

    std::vector<BSONObj> runListCollections(OperationContext* opCtx,
                                            const DatabaseName& db,
                                            bool addPrimaryShard) final;

    std::vector<FieldPath> collectDocumentKeyFieldsActingAsRouter(
        OperationContext*,
        const NamespaceString&,
        RoutingContext* routingCtx = nullptr) const final;

    boost::optional<Document> lookupSingleDocument(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const NamespaceString& nss,
        boost::optional<UUID> collectionUUID,
        const Document& documentKey,
        boost::optional<BSONObj> readConcern) final;

    Status insert(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                  const NamespaceString& ns,
                  std::unique_ptr<write_ops::InsertCommandRequest> insertCommand,
                  const WriteConcernOptions& wc,
                  boost::optional<OID> targetEpoch) override;

    Status insertTimeseries(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                            const NamespaceString& ns,
                            std::unique_ptr<write_ops::InsertCommandRequest> insertCommand,
                            const WriteConcernOptions& wc,
                            boost::optional<OID> targetEpoch) override;

    StatusWith<UpdateResult> update(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                    const NamespaceString& ns,
                                    std::unique_ptr<write_ops::UpdateCommandRequest> updateCommand,
                                    const WriteConcernOptions& wc,
                                    UpsertType upsert,
                                    bool multi,
                                    boost::optional<OID> targetEpoch) override;

    void renameIfOptionsAndIndexesHaveNotChanged(
        OperationContext* opCtx,
        const NamespaceString& sourceNs,
        const NamespaceString& targetNs,
        bool dropTarget,
        bool stayTemp,
        const BSONObj& originalCollectionOptions,
        const std::list<BSONObj>& originalIndexes) override;

    void createCollection(OperationContext* opCtx,
                          const DatabaseName& dbName,
                          const BSONObj& cmdObj) override;

    void createTempCollection(OperationContext* opCtx,
                              const NamespaceString& nss,
                              const BSONObj& collectionOptions,
                              boost::optional<ShardId> dataShard) override;

    void createTimeseriesView(OperationContext* opCtx,
                              const NamespaceString& ns,
                              const BSONObj& cmdObj,
                              const TimeseriesOptions& userOpts) override;

    void dropCollection(OperationContext* opCtx, const NamespaceString& collection) override;

    void dropTempCollection(OperationContext* opCtx, const NamespaceString& nss) override;

    void createIndexesOnEmptyCollection(OperationContext* opCtx,
                                        const NamespaceString& ns,
                                        const std::vector<BSONObj>& indexSpecs) override;

    std::unique_ptr<ScopedExpectUnshardedCollection> expectUnshardedCollectionInScope(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const boost::optional<DatabaseVersion>& dbVersion) override {
        class ScopedExpectUnshardedCollectionNoop : public ScopedExpectUnshardedCollection {
        public:
            ScopedExpectUnshardedCollectionNoop() = default;
        };

        return std::make_unique<ScopedExpectUnshardedCollectionNoop>();
    }

protected:
    // This constructor is marked as protected in order to prevent instantiation since this
    // interface is designed to have a concrete process interface for each possible
    // configuration of a mongod.
    NonShardServerProcessInterface(std::shared_ptr<executor::TaskExecutor> exec)
        : CommonMongodProcessInterface(std::move(exec)) {}
};

}  // namespace mongo
