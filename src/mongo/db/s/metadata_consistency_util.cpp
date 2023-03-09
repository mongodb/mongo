/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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


#include "mongo/db/s/metadata_consistency_util.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/cursor_manager.h"
#include "mongo/db/exec/queued_data_stage.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/metadata_consistency_types_gen.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/plan_executor_factory.h"

namespace mongo {
namespace metadata_consistency_util {

namespace {
void _appendHiddenUnshardedCollectionInconsistency(
    const ShardId& shardId,
    const NamespaceString& localNss,
    const UUID& localUUID,
    std::vector<MetadataInconsistencyItem>& inconsistencies) {
    MetadataInconsistencyItem val;
    val.setNs(localNss);
    val.setType(MetadataInconsistencyTypeEnum::kHiddenUnshardedCollection);
    val.setShard(shardId);
    val.setInfo(BSON("description"
                     << "Unsharded collection found on shard differnt from db primary shard"
                     << "localUUID" << localUUID));
    inconsistencies.emplace_back(std::move(val));
}

void _appendUUIDMismatchInconsistency(const ShardId& shardId,
                                      const NamespaceString& localNss,
                                      const UUID& localUUID,
                                      const UUID& UUID,
                                      bool isLocalCollectionSharded,
                                      std::vector<MetadataInconsistencyItem>& inconsistencies) {
    MetadataInconsistencyItem val;
    val.setNs(localNss);
    val.setType(MetadataInconsistencyTypeEnum::kUUIDMismatch);
    val.setShard(shardId);
    val.setInfo(BSON("description"
                     << "Found collection on non primary shard with mismatching UUID"
                     << "localUUID" << localUUID << "UUID" << UUID
                     << "shardThinkCollectionIsSharded" << isLocalCollectionSharded));
    inconsistencies.emplace_back(std::move(val));
}
}  // namespace


std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> makeQueuedPlanExecutor(
    OperationContext* opCtx,
    const std::vector<MetadataInconsistencyItem>& inconsistencies,
    const NamespaceString& nss) {

    auto expCtx =
        make_intrusive<ExpressionContext>(opCtx, std::unique_ptr<CollatorInterface>(nullptr), nss);
    auto ws = std::make_unique<WorkingSet>();
    auto root = std::make_unique<QueuedDataStage>(expCtx.get(), ws.get());

    for (auto&& inconsistency : inconsistencies) {
        WorkingSetID id = ws->allocate();
        WorkingSetMember* member = ws->get(id);
        member->keyData.clear();
        member->recordId = RecordId();
        member->resetDocument(SnapshotId(), inconsistency.toBSON().getOwned());
        member->transitionToOwnedObj();
        root->pushBack(id);
    }

    return uassertStatusOK(
        plan_executor_factory::make(expCtx,
                                    std::move(ws),
                                    std::move(root),
                                    &CollectionPtr::null,
                                    PlanYieldPolicy::YieldPolicy::NO_YIELD,
                                    false, /* whether returned BSON must be owned */
                                    nss));
}


CursorInitialReply createInitialCursorReplyMongod(OperationContext* opCtx,
                                                  ClientCursorParams&& cursorParams,
                                                  long long batchSize) {
    auto& exec = cursorParams.exec;
    auto& nss = cursorParams.nss;

    std::vector<BSONObj> firstBatch;
    size_t bytesBuffered = 0;
    for (long long objCount = 0; objCount < batchSize; objCount++) {
        BSONObj nextDoc;
        PlanExecutor::ExecState state = exec->getNext(&nextDoc, nullptr);
        if (state == PlanExecutor::IS_EOF) {
            break;
        }
        invariant(state == PlanExecutor::ADVANCED);

        // If we can't fit this result inside the current batch, then we stash it for
        // later.
        if (!FindCommon::haveSpaceForNext(nextDoc, objCount, bytesBuffered)) {
            exec->stashResult(nextDoc);
            break;
        }

        bytesBuffered += nextDoc.objsize();
        firstBatch.push_back(std::move(nextDoc));
    }

    if (exec->isEOF()) {
        CursorInitialReply resp;
        InitialResponseCursor initRespCursor{std::move(firstBatch)};
        initRespCursor.setResponseCursorBase({0LL /* cursorId */, nss});
        resp.setCursor(std::move(initRespCursor));
        return resp;
    }

    exec->saveState();
    exec->detachFromOperationContext();

    auto pinnedCursor = CursorManager::get(opCtx)->registerCursor(opCtx, std::move(cursorParams));

    pinnedCursor->incNBatches();
    pinnedCursor->incNReturnedSoFar(firstBatch.size());

    CursorInitialReply resp;
    InitialResponseCursor initRespCursor{std::move(firstBatch)};
    initRespCursor.setResponseCursorBase({pinnedCursor.getCursor()->cursorid(), nss});
    resp.setCursor(std::move(initRespCursor));
    return resp;
}

std::vector<MetadataInconsistencyItem> checkCollectionMetadataInconsistencies(
    OperationContext* opCtx,
    const ShardId& shardId,
    const ShardId& primaryShardId,
    const std::vector<CollectionType>& catalogClientCollections,
    const std::vector<CollectionPtr>& localCollections) {
    std::vector<MetadataInconsistencyItem> inconsistencies;
    auto itLocalCollections = localCollections.begin();
    auto itCatalogCollections = catalogClientCollections.begin();
    while (itLocalCollections != localCollections.end() &&
           itCatalogCollections != catalogClientCollections.end()) {
        const auto& localColl = itLocalCollections->get();
        const auto& localUUID = localColl->uuid();
        const auto& localNss = localColl->ns();
        const auto& nss = itCatalogCollections->getNss();

        const auto cmp = nss.coll().compare(localNss.coll());
        if (cmp < 0) {
            // Case where we have found a collection in the catalog client that it is not in the
            // local catalog.
            itCatalogCollections++;
        } else if (cmp == 0) {
            // Case where we have found same collection in the catalog client than in the local
            // catalog.
            const auto& UUID = itCatalogCollections->getUuid();
            if (UUID != localUUID) {
                _appendUUIDMismatchInconsistency(shardId,
                                                 localNss,
                                                 localUUID,
                                                 UUID,
                                                 itLocalCollections->isSharded(),
                                                 inconsistencies);
            }
            itLocalCollections++;
            itCatalogCollections++;
        } else {
            // Case where we have found a local collection that is not in the catalog client.
            if (shardId != primaryShardId) {
                _appendHiddenUnshardedCollectionInconsistency(
                    shardId, localNss, localUUID, inconsistencies);
            }
            itLocalCollections++;
        }
    }

    // Case where we have found more local collections than in the catalog client. It is a
    // hidden unsharded collection inconsistency if we are not the db primary shard.
    while (itLocalCollections != localCollections.end() && shardId != primaryShardId) {
        const auto localColl = itLocalCollections->get();
        _appendHiddenUnshardedCollectionInconsistency(
            shardId, localColl->ns(), localColl->uuid(), inconsistencies);
        itLocalCollections++;
    }

    return inconsistencies;
}

}  // namespace metadata_consistency_util
}  // namespace mongo
