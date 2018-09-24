/**
 * Copyright (C) 2018 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
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
#include "mongo/db/pipeline/document_source_cursor.h"
#include "mongo/db/pipeline/pipeline_d.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/session_catalog.h"
#include "mongo/db/stats/fill_locker_info.h"
#include "mongo/db/stats/storage_stats.h"
#include "mongo/db/storage/backup_cursor_hooks.h"
#include "mongo/db/transaction_participant.h"
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

/**
 * Builds an ordered insert op on namespace 'nss' and documents to be written 'objs'.
 */
Insert buildInsertOp(const NamespaceString& nss,
                     std::vector<BSONObj>&& objs,
                     bool bypassDocValidation) {
    Insert insertOp(nss);
    insertOp.setDocuments(std::move(objs));
    insertOp.setWriteCommandBase([&] {
        write_ops::WriteCommandBase wcb;
        wcb.setOrdered(true);
        wcb.setBypassDocumentValidation(bypassDocValidation);
        return wcb;
    }());
    return insertOp;
}

/**
 * Builds an ordered update op on namespace 'nss' with update entries {q: <queries>, u: <updates>}.
 *
 * Note that 'queries' and 'updates' must be the same length.
 */
Update buildUpdateOp(const NamespaceString& nss,
                     std::vector<BSONObj>&& queries,
                     std::vector<BSONObj>&& updates,
                     bool upsert,
                     bool multi,
                     bool bypassDocValidation) {
    Update updateOp(nss);
    updateOp.setUpdates([&] {
        std::vector<UpdateOpEntry> updateEntries;
        for (size_t index = 0; index < queries.size(); ++index) {
            updateEntries.push_back([&] {
                UpdateOpEntry entry;
                entry.setQ(std::move(queries[index]));
                entry.setU(std::move(updates[index]));
                entry.setUpsert(upsert);
                entry.setMulti(multi);
                return entry;
            }());
        }
        return updateEntries;
    }());
    updateOp.setWriteCommandBase([&] {
        write_ops::WriteCommandBase wcb;
        wcb.setOrdered(true);
        wcb.setBypassDocumentValidation(bypassDocValidation);
        return wcb;
    }());
    return updateOp;
}

// Returns true if the field names of 'keyPattern' are exactly those in 'uniqueKeyPaths', and each
// of the elements of 'keyPattern' is numeric, i.e. not "text", "$**", or any other special type of
// index.
bool keyPatternNamesExactPaths(const BSONObj& keyPattern,
                               const std::set<FieldPath>& uniqueKeyPaths) {
    size_t nFieldsMatched = 0;
    for (auto&& elem : keyPattern) {
        if (!elem.isNumber()) {
            return false;
        }
        if (uniqueKeyPaths.find(elem.fieldNameStringData()) == uniqueKeyPaths.end()) {
            return false;
        }
        ++nFieldsMatched;
    }
    return nFieldsMatched == uniqueKeyPaths.size();
}

bool supportsUniqueKey(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                       const IndexCatalogEntry* index,
                       const std::set<FieldPath>& uniqueKeyPaths) {
    return (index->descriptor()->unique() && !index->descriptor()->isPartial() &&
            keyPatternNamesExactPaths(index->descriptor()->keyPattern(), uniqueKeyPaths) &&
            CollatorInterface::collatorsMatch(index->getCollator(), expCtx->getCollator()));
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

    auto scm = [opCtx, &nss]() -> ScopedCollectionMetadata {
        AutoGetCollection autoColl(opCtx, nss, MODE_IS);
        return CollectionShardingState::get(opCtx, nss)->getMetadata(opCtx);
    }();

    // If the UUID is set in 'nssOrUuid', check that the UUID in the ScopedCollectionMetadata
    // matches. Otherwise, this implies that the collection has been dropped and recreated as
    // sharded.
    if (!scm->isSharded() || (uuid && !scm->uuidMatches(*uuid))) {
        return {{"_id"}, false};
    }

    // Unpack the shard key.
    std::vector<FieldPath> result;
    bool gotId = false;
    for (auto& field : scm->getKeyPatternFields()) {
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
                                       std::vector<BSONObj>&& objs) {
    BatchedCommandResponse response;
    BatchWriteExecStats stats;

    ClusterWriter::write(
        expCtx->opCtx,
        BatchedCommandRequest(buildInsertOp(ns, std::move(objs), expCtx->bypassDocumentValidation)),
        &stats,
        &response);
    // TODO SERVER-35403: Add more context for which shard produced the error.
    uassertStatusOKWithContext(response.toStatus(), "Insert failed: ");
}

void MongoInterfaceShardServer::update(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                       const NamespaceString& ns,
                                       std::vector<BSONObj>&& queries,
                                       std::vector<BSONObj>&& updates,
                                       bool upsert,
                                       bool multi) {
    BatchedCommandResponse response;
    BatchWriteExecStats stats;
    ClusterWriter::write(expCtx->opCtx,
                         BatchedCommandRequest(buildUpdateOp(ns,
                                                             std::move(queries),
                                                             std::move(updates),
                                                             upsert,
                                                             multi,
                                                             expCtx->bypassDocumentValidation)),
                         &stats,
                         &response);
    // TODO SERVER-35403: Add more context for which shard produced the error.
    uassertStatusOKWithContext(response.toStatus(), "Update failed: ");
}

}  // namespace mongo
