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

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/mongos_process_interface.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/curop.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/sharded_agg_helpers.h"
#include "mongo/db/query/collation/collation_spec.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/s/scoped_collection_metadata.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/cluster_cursor_manager.h"
#include "mongo/s/query/establish_cursors.h"
#include "mongo/s/query/router_exec_stage.h"
#include "mongo/s/transaction_router.h"
#include "mongo/util/fail_point.h"

namespace mongo {

using boost::intrusive_ptr;
using std::shared_ptr;
using std::string;
using std::unique_ptr;

namespace {

/**
 * Determines the single shard to which the given query will be targeted, and its associated
 * shardVersion. Throws if the query targets more than one shard.
 */
std::pair<ShardId, ChunkVersion> getSingleTargetedShardForQuery(
    OperationContext* opCtx, const CachedCollectionRoutingInfo& routingInfo, BSONObj query) {
    if (auto chunkMgr = routingInfo.cm()) {
        std::set<ShardId> shardIds;
        chunkMgr->getShardIdsForQuery(opCtx, query, CollationSpec::kSimpleSpec, &shardIds);
        uassert(ErrorCodes::InternalError,
                str::stream() << "Unable to target lookup query to a single shard: "
                              << query.toString(),
                shardIds.size() == 1u);
        return {*shardIds.begin(), chunkMgr->getVersion(*shardIds.begin())};
    }

    return {routingInfo.db().primaryId(), ChunkVersion::UNSHARDED()};
}

/**
 * Returns the routing information for the namespace set on the passed ExpressionContext. Also
 * verifies that the ExpressionContext's UUID, if present, matches that of the routing table entry.
 */
StatusWith<CachedCollectionRoutingInfo> getCollectionRoutingInfo(
    const intrusive_ptr<ExpressionContext>& expCtx) {
    auto catalogCache = Grid::get(expCtx->opCtx)->catalogCache();
    auto swRoutingInfo = catalogCache->getCollectionRoutingInfo(expCtx->opCtx, expCtx->ns);
    // Additionally check that the ExpressionContext's UUID matches the collection routing info.
    if (swRoutingInfo.isOK() && expCtx->uuid && swRoutingInfo.getValue().cm()) {
        if (!swRoutingInfo.getValue().cm()->uuidMatches(*expCtx->uuid)) {
            return {ErrorCodes::NamespaceNotFound,
                    str::stream() << "The UUID of collection " << expCtx->ns.ns()
                                  << " changed; it may have been dropped and re-created."};
        }
    }
    return swRoutingInfo;
}

bool supportsUniqueKey(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                       const BSONObj& index,
                       const std::set<FieldPath>& uniqueKeyPaths) {
    // Retrieve the collation from the index, or default to the simple collation.
    const auto collation = uassertStatusOK(
        CollatorFactoryInterface::get(expCtx->opCtx->getServiceContext())
            ->makeFromBSON(index.hasField(IndexDescriptor::kCollationFieldName)
                               ? index.getObjectField(IndexDescriptor::kCollationFieldName)
                               : CollationSpec::kSimpleSpec));

    // SERVER-5335: The _id index does not report to be unique, but in fact is unique.
    auto isIdIndex = index[IndexDescriptor::kIndexNameFieldName].String() == "_id_";
    return (isIdIndex || index.getBoolField(IndexDescriptor::kUniqueFieldName)) &&
        !index.hasField(IndexDescriptor::kPartialFilterExprFieldName) &&
        MongoProcessCommon::keyPatternNamesExactPaths(
               index.getObjectField(IndexDescriptor::kKeyPatternFieldName), uniqueKeyPaths) &&
        CollatorInterface::collatorsMatch(collation.get(), expCtx->getCollator());
}

}  // namespace

std::unique_ptr<Pipeline, PipelineDeleter> MongoSInterface::makePipeline(
    const std::vector<BSONObj>& rawPipeline,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const MakePipelineOptions pipelineOptions) {
    // Explain is not supported for auxiliary lookups.
    invariant(!expCtx->explain);

    auto pipeline = uassertStatusOK(Pipeline::parse(rawPipeline, expCtx));
    if (pipelineOptions.optimize) {
        pipeline->optimizePipeline();
    }
    if (pipelineOptions.attachCursorSource) {
        // 'attachCursorSourceToPipeline' handles any complexity related to sharding.
        pipeline = attachCursorSourceToPipeline(expCtx, pipeline.release());
    }

    return pipeline;
}

std::unique_ptr<Pipeline, PipelineDeleter> MongoSInterface::attachCursorSourceToPipeline(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, Pipeline* ownedPipeline) {
    return sharded_agg_helpers::targetShardsAndAddMergeCursors(expCtx, ownedPipeline);
}

boost::optional<Document> MongoSInterface::lookupSingleDocument(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& nss,
    UUID collectionUUID,
    const Document& filter,
    boost::optional<BSONObj> readConcern,
    bool allowSpeculativeMajorityRead) {
    auto foreignExpCtx = expCtx->copyWith(nss, collectionUUID);

    // Create the find command to be dispatched to the shard in order to return the post-change
    // document.
    auto filterObj = filter.toBson();
    BSONObjBuilder cmdBuilder;
    bool findCmdIsByUuid(foreignExpCtx->uuid);
    if (findCmdIsByUuid) {
        foreignExpCtx->uuid->appendToBuilder(&cmdBuilder, "find");
    } else {
        cmdBuilder.append("find", nss.coll());
    }
    cmdBuilder.append("filter", filterObj);
    cmdBuilder.append("comment", expCtx->comment);
    if (readConcern) {
        cmdBuilder.append(repl::ReadConcernArgs::kReadConcernFieldName, *readConcern);
    }
    if (allowSpeculativeMajorityRead) {
        cmdBuilder.append("allowSpeculativeMajorityRead", true);
    }

    auto shardResult = std::vector<RemoteCursor>();
    auto findCmd = cmdBuilder.obj();
    size_t numAttempts = 0;
    while (++numAttempts <= kMaxNumStaleVersionRetries) {
        // Verify that the collection exists, with the correct UUID.
        auto catalogCache = Grid::get(expCtx->opCtx)->catalogCache();
        auto swRoutingInfo = getCollectionRoutingInfo(foreignExpCtx);
        if (swRoutingInfo == ErrorCodes::NamespaceNotFound) {
            return boost::none;
        }
        auto routingInfo = uassertStatusOK(std::move(swRoutingInfo));
        if (findCmdIsByUuid && routingInfo.cm()) {
            // Find by UUID and shard versioning do not work together (SERVER-31946).  In the
            // sharded case we've already checked the UUID, so find by namespace is safe.  In the
            // unlikely case that the collection has been deleted and a new collection with the same
            // name created through a different mongos, the shard version will be detected as stale,
            // as shard versions contain an 'epoch' field unique to the collection.
            findCmd = findCmd.addField(BSON("find" << nss.coll()).firstElement());
            findCmdIsByUuid = false;
        }

        // Get the ID and version of the single shard to which this query will be sent.
        auto shardInfo = getSingleTargetedShardForQuery(expCtx->opCtx, routingInfo, filterObj);

        // Dispatch the request. This will only be sent to a single shard and only a single result
        // will be returned. The 'establishCursors' method conveniently prepares the result into a
        // cursor response for us.
        try {
            shardResult = establishCursors(
                expCtx->opCtx,
                Grid::get(expCtx->opCtx)->getExecutorPool()->getArbitraryExecutor(),
                nss,
                ReadPreferenceSetting::get(expCtx->opCtx),
                {{shardInfo.first, appendShardVersion(findCmd, shardInfo.second)}},
                false);
            break;
        } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
            // If it's an unsharded collection which has been deleted and re-created, we may get a
            // NamespaceNotFound error when looking up by UUID.
            return boost::none;
        } catch (const ExceptionForCat<ErrorCategory::StaleShardVersionError>&) {
            // If we hit a stale shardVersion exception, invalidate the routing table cache.
            catalogCache->onStaleShardVersion(std::move(routingInfo));
            continue;  // Try again if allowed.
        }
        break;  // Success!
    }

    invariant(shardResult.size() == 1u);

    auto& cursor = shardResult.front().getCursorResponse();
    auto& batch = cursor.getBatch();

    // We should have at most 1 result, and the cursor should be exhausted.
    uassert(ErrorCodes::InternalError,
            str::stream() << "Shard cursor was unexpectedly open after lookup: "
                          << shardResult.front().getHostAndPort()
                          << ", id: "
                          << cursor.getCursorId(),
            cursor.getCursorId() == 0);
    uassert(ErrorCodes::TooManyMatchingDocuments,
            str::stream() << "found more than one document matching " << filter.toString() << " ["
                          << batch.begin()->toString()
                          << ", "
                          << std::next(batch.begin())->toString()
                          << "]",
            batch.size() <= 1u);

    return (!batch.empty() ? Document(batch.front()) : boost::optional<Document>{});
}

BSONObj MongoSInterface::_reportCurrentOpForClient(OperationContext* opCtx,
                                                   Client* client,
                                                   CurrentOpTruncateMode truncateOps) const {
    BSONObjBuilder builder;

    CurOp::reportCurrentOpForClient(
        opCtx, client, (truncateOps == CurrentOpTruncateMode::kTruncateOps), &builder);

    return builder.obj();
}

std::vector<GenericCursor> MongoSInterface::getIdleCursors(
    const intrusive_ptr<ExpressionContext>& expCtx, CurrentOpUserMode userMode) const {
    invariant(hasGlobalServiceContext());
    auto cursorManager = Grid::get(expCtx->opCtx->getServiceContext())->getCursorManager();
    invariant(cursorManager);
    return cursorManager->getIdleCursors(expCtx->opCtx, userMode);
}

bool MongoSInterface::isSharded(OperationContext* opCtx, const NamespaceString& nss) {
    auto routingInfo = Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss);
    return routingInfo.isOK() && routingInfo.getValue().cm();
}

bool MongoSInterface::uniqueKeyIsSupportedByIndex(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& nss,
    const std::set<FieldPath>& uniqueKeyPaths) const {
    const auto opCtx = expCtx->opCtx;
    const auto routingInfo =
        uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss));

    // Run an exhaustive listIndexes against the primary shard only.
    auto response = routingInfo.db().primary()->runExhaustiveCursorCommand(
        opCtx,
        ReadPreferenceSetting::get(opCtx),
        nss.db().toString(),
        BSON("listIndexes" << nss.coll()),
        opCtx->hasDeadline() ? opCtx->getRemainingMaxTimeMillis() : Milliseconds(-1));

    // If the namespace does not exist, then the unique key *must* be _id only.
    if (response.getStatus() == ErrorCodes::NamespaceNotFound) {
        return uniqueKeyPaths == std::set<FieldPath>{"_id"};
    }
    uassertStatusOK(response);

    const auto& indexes = response.getValue().docs;
    return std::any_of(
        indexes.begin(), indexes.end(), [&expCtx, &uniqueKeyPaths](const auto& index) {
            return supportsUniqueKey(expCtx, index, uniqueKeyPaths);
        });
}

}  // namespace mongo
