
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/process_interface_shardsvr.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/catalog/uuid_catalog.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/ops/write_ops_exec.h"
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/grid.h"
#include "mongo/s/write_ops/cluster_write.h"
#include "mongo/util/log.h"

namespace mongo {

using boost::intrusive_ptr;
using std::shared_ptr;
using std::string;
using std::unique_ptr;
using write_ops::Insert;
using write_ops::Update;
using write_ops::UpdateOpEntry;

namespace {

// Attaches the write concern to the given batch request. If it looks like 'writeConcern' has
// been default initialized to {w: 0, wtimeout: 0} then we do not bother attaching it.
void attachWriteConcern(BatchedCommandRequest* request, const WriteConcernOptions& writeConcern) {
    if (!writeConcern.wMode.empty() || writeConcern.wNumNodes > 0) {
        request->setWriteConcern(writeConcern.toBSON());
    }
}

}  // namespace

std::pair<std::vector<FieldPath>, bool> MongoInterfaceShardServer::collectDocumentKeyFields(
    OperationContext* opCtx, NamespaceStringOrUUID nssOrUUID) const {
    invariant(serverGlobalParams.clusterRole == ClusterRole::ShardServer);

    boost::optional<UUID> uuid;
    NamespaceString nss;
    if (nssOrUUID.uuid()) {
        uuid = *(nssOrUUID.uuid());
        nss = UUIDCatalog::get(opCtx).lookupNSSByUUID(*uuid);
        // An empty namespace indicates that the collection has been dropped. Treat it as unsharded
        // and mark the fields as final.
        if (nss.isEmpty()) {
            return {{"_id"}, true};
        }
    } else if (nssOrUUID.nss()) {
        nss = *(nssOrUUID.nss());
    }

    // Before taking a collection lock to retrieve the shard key fields, consult the catalog cache
    // to determine whether the collection is sharded in the first place.
    auto catalogCache = Grid::get(opCtx)->catalogCache();

    const bool collectionIsSharded = catalogCache && [&]() {
        auto routingInfo = catalogCache->getCollectionRoutingInfo(opCtx, nss);
        return routingInfo.isOK() && routingInfo.getValue().cm();
    }();

    // Collection exists and is not sharded, mark as not final.
    if (!collectionIsSharded) {
        return {{"_id"}, false};
    }

    const auto metadata = [opCtx, &nss]() {
        AutoGetCollection autoColl(opCtx, nss, MODE_IS);
        return CollectionShardingState::get(opCtx, nss)->getCurrentMetadata();
    }();

    // If the UUID is set in 'nssOrUuid', check that the UUID in the ScopedCollectionMetadata
    // matches. Otherwise, this implies that the collection has been dropped and recreated as
    // sharded.
    if (!metadata->isSharded() || (uuid && !metadata->uuidMatches(*uuid))) {
        return {{"_id"}, false};
    }

    // Unpack the shard key.
    std::vector<FieldPath> result;
    bool gotId = false;
    for (auto& field : metadata->getKeyPatternFields()) {
        result.emplace_back(field->dottedField());
        gotId |= (result.back().fullPath() == "_id");
    }
    if (!gotId) {  // If not part of the shard key, "_id" comes last.
        result.emplace_back("_id");
    }
    // Collection is now sharded so the document key fields will never change, mark as final.
    return {result, true};
}

void MongoInterfaceShardServer::insert(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                       const NamespaceString& ns,
                                       std::vector<BSONObj>&& objs,
                                       const WriteConcernOptions& wc,
                                       boost::optional<OID> targetEpoch) {
    BatchedCommandResponse response;
    BatchWriteExecStats stats;

    BatchedCommandRequest insertCommand(
        buildInsertOp(ns, std::move(objs), expCtx->bypassDocumentValidation));

    // If applicable, attach a write concern to the batched command request.
    attachWriteConcern(&insertCommand, wc);

    ClusterWriter::write(expCtx->opCtx, insertCommand, &stats, &response, targetEpoch);

    // TODO SERVER-35403: Add more context for which shard produced the error.
    uassertStatusOKWithContext(response.toStatus(), "Insert failed: ");
}

void MongoInterfaceShardServer::update(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                       const NamespaceString& ns,
                                       std::vector<BSONObj>&& queries,
                                       std::vector<BSONObj>&& updates,
                                       const WriteConcernOptions& wc,
                                       bool upsert,
                                       bool multi,
                                       boost::optional<OID> targetEpoch) {
    BatchedCommandResponse response;
    BatchWriteExecStats stats;

    BatchedCommandRequest updateCommand(buildUpdateOp(ns,
                                                      std::move(queries),
                                                      std::move(updates),
                                                      upsert,
                                                      multi,
                                                      expCtx->bypassDocumentValidation));

    // If applicable, attach a write concern to the batched command request.
    attachWriteConcern(&updateCommand, wc);

    ClusterWriter::write(expCtx->opCtx, updateCommand, &stats, &response, targetEpoch);

    // TODO SERVER-35403: Add more context for which shard produced the error.
    uassertStatusOKWithContext(response.toStatus(), "Update failed: ");
}

}  // namespace mongo
