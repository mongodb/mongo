/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/collection_sharding_state.h"

#include "mongo/db/concurrency/lock_state.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/collection_metadata.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/s/chunk_version.h"

namespace mongo {

CollectionShardingState::CollectionShardingState(
    NamespaceString nss, std::unique_ptr<CollectionMetadata> initialMetadata)
    : _nss(std::move(nss)), _metadata(std::move(initialMetadata)) {}

CollectionShardingState::~CollectionShardingState() = default;

CollectionShardingState* CollectionShardingState::get(OperationContext* txn,
                                                      const NamespaceString& nss) {
    return CollectionShardingState::get(txn, nss.ns());
}

CollectionShardingState* CollectionShardingState::get(OperationContext* txn,
                                                      const std::string& ns) {
    // Collection lock must be held to have a reference to the collection's sharding state
    dassert(txn->lockState()->isCollectionLockedForMode(ns, MODE_IS));

    ShardingState* const shardingState = ShardingState::get(txn);
    return shardingState->getNS(ns);
}

void CollectionShardingState::setMetadata(std::shared_ptr<CollectionMetadata> newMetadata) {
    invariant(newMetadata);
    _metadata = std::move(newMetadata);
}

bool CollectionShardingState::isDocumentInMigratingChunk(OperationContext* txn,
                                                         const BSONObj& doc) {
    dassert(txn->lockState()->isCollectionLockedForMode(_nss.ns(), MODE_IX));

    return ShardingState::get(txn)->migrationSourceManager()->isInMigratingChunk(_nss, doc);
}

void CollectionShardingState::onInsertOp(OperationContext* txn, const BSONObj& insertedDoc) {
    dassert(txn->lockState()->isCollectionLockedForMode(_nss.ns(), MODE_IX));

    ShardingState::get(txn)->migrationSourceManager()->logInsertOp(
        txn, _nss.ns().c_str(), insertedDoc);
}

void CollectionShardingState::onUpdateOp(OperationContext* txn, const BSONObj& updatedDoc) {
    dassert(txn->lockState()->isCollectionLockedForMode(_nss.ns(), MODE_IX));

    ShardingState::get(txn)->migrationSourceManager()->logUpdateOp(
        txn, _nss.ns().c_str(), updatedDoc);
}

void CollectionShardingState::onDeleteOp(OperationContext* txn, const BSONObj& deletedDocId) {
    dassert(txn->lockState()->isCollectionLockedForMode(_nss.ns(), MODE_IX));

    ShardingState::get(txn)->migrationSourceManager()->logDeleteOp(
        txn, _nss.ns().c_str(), deletedDocId);
}

}  // namespace mongo
