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

#include "mongo/db/exec/write_stage_common.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/exec/shard_filterer_impl.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/logv2/redaction.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kWrite


namespace mongo {

namespace write_stage_common {

PreWriteFilter::PreWriteFilter(OperationContext* opCtx, NamespaceString nss)
    : _opCtx(opCtx), _nss(std::move(nss)), _skipFiltering([&] {
          // Always allow writes on replica sets.
          if (serverGlobalParams.clusterRole == ClusterRole::None) {
              return true;
          }

          // Always allow writes on standalone and secondary nodes.
          const auto replCoord{repl::ReplicationCoordinator::get(opCtx)};
          return !replCoord->canAcceptWritesForDatabase(opCtx, NamespaceString::kAdminDb);
      }()) {}

PreWriteFilter::Action PreWriteFilter::computeAction(const Document& doc) {
    if (_skipFiltering) {
        // Secondaries do not apply any filtering logic as the primary already did.
        return Action::kWrite;
    }

    const auto docBelongsToMe = _documentBelongsToMe(doc.toBson());
    if (docBelongsToMe)
        return Action::kWrite;
    else
        return OperationShardingState::isComingFromRouter(_opCtx) ? Action::kSkip
                                                                  : Action::kWriteAsFromMigrate;
}

bool PreWriteFilter::_documentBelongsToMe(const BSONObj& doc) {
    if (!_shardFilterer) {
        _shardFilterer = [&] {
            const auto css{CollectionShardingState::get(_opCtx, _nss)};
            return std::make_unique<ShardFiltererImpl>(css->getOwnershipFilter(
                _opCtx,
                CollectionShardingState::OrphanCleanupPolicy::kAllowOrphanCleanup,
                true /*supportNonVersionedOperations*/));
        }();
    }

    const auto docBelongsToMe = _shardFilterer->documentBelongsToMe(doc);
    uassert(ErrorCodes::ShardKeyNotFound,
            str::stream() << "No shard key found in document " << redact(doc)
                          << " and shard key pattern "
                          << _shardFilterer->getKeyPattern().toString(),
            docBelongsToMe != ShardFilterer::DocumentBelongsResult::kNoShardKey);

    if (docBelongsToMe == ShardFilterer::DocumentBelongsResult::kBelongs) {
        return true;
    } else {
        invariant(docBelongsToMe == ShardFilterer::DocumentBelongsResult::kDoesNotBelong);
        return false;
    }
}

void PreWriteFilter::restoreState() {
    _shardFilterer.reset();
}

bool ensureStillMatches(const CollectionPtr& collection,
                        OperationContext* opCtx,
                        WorkingSet* ws,
                        WorkingSetID id,
                        const CanonicalQuery* cq) {
    // If the snapshot changed, then we have to make sure we have the latest copy of the doc and
    // that it still matches.
    WorkingSetMember* member = ws->get(id);
    if (opCtx->recoveryUnit()->getSnapshotId() != member->doc.snapshotId()) {
        std::unique_ptr<SeekableRecordCursor> cursor(collection->getCursor(opCtx));

        if (!WorkingSetCommon::fetch(opCtx, ws, id, cursor.get(), collection, collection->ns())) {
            // Doc is already deleted.
            return false;
        }

        // Make sure the re-fetched doc still matches the predicate.
        if (cq && !cq->root()->matchesBSON(member->doc.value().toBson(), nullptr)) {
            // No longer matches.
            return false;
        }

        // Ensure that the BSONObj underlying the WorkingSetMember is owned because the cursor's
        // destructor is allowed to free the memory.
        member->makeObjOwnedIfNeeded();
    }
    return true;
}

}  // namespace write_stage_common
}  // namespace mongo
