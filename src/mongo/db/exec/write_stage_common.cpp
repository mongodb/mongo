// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/write_stage_common.h"

#include "mongo/db/database_name.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/classic/working_set_common.h"
#include "mongo/db/exec/matcher/matcher.h"
#include "mongo/db/exec/shard_filterer_impl.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_options.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_state.h"
#include "mongo/db/shard_role/shard_catalog/operation_sharding_state.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/logv2/log.h"
#include "mongo/util/str.h"

#include <string_view>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kWrite


namespace mongo {

namespace write_stage_common {

PreWriteFilter::PreWriteFilter(OperationContext* opCtx, NamespaceString nss)
    : _opCtx(opCtx), _nss(std::move(nss)), _skipFiltering([&] {
          // Allow writes on standalone and replica set.
          if (serverGlobalParams.clusterRole.has(ClusterRole::None)) {
              return true;
          }

          // Only the primary node of a shard that is a replica set should run this filter.
          const auto replCoord = repl::ReplicationCoordinator::get(opCtx);
          return !replCoord->getSettings().isReplSet() ||
              !replCoord->canAcceptWritesForDatabase(opCtx, DatabaseName::kAdmin);
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
        return OperationShardingState::isShardingAware(_opCtx) ? Action::kSkip
                                                               : Action::kWriteAsFromMigrate;
}

bool PreWriteFilter::_documentBelongsToMe(const BSONObj& doc) {
    if (!_shardFilterer) {
        _shardFilterer = [&] {
            auto scopedCss =
                CollectionShardingState::assertCollectionLockedAndAcquire(_opCtx, _nss);
            return std::make_unique<ShardFiltererImpl>(scopedCss->getOwnershipFilter(
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

void PreWriteFilter::logSkippingDocument(const Document& doc,
                                         std::string_view opKind,
                                         const NamespaceString& collNs) {
    LOGV2_DEBUG(5983201,
                3,
                "Skipping the operation to orphan document to prevent a wrong change "
                "stream event",
                "op"_attr = opKind,
                logAttrs(collNs),
                "record"_attr = redact(doc.toString()));
}

void PreWriteFilter::logFromMigrate(const Document& doc,
                                    std::string_view opKind,
                                    const NamespaceString& collNs) {
    LOGV2_DEBUG(6184700,
                3,
                "Marking the operation to orphan document with the fromMigrate flag to "
                "prevent a wrong change stream event",
                "op"_attr = opKind,
                logAttrs(collNs),
                "record"_attr = redact(doc.toString()));
}

bool ensureStillMatches(const CollectionPtr& collection,
                        OperationContext* opCtx,
                        WorkingSet* ws,
                        WorkingSetID id,
                        const CanonicalQuery* cq,
                        size_t& docsFetchedCounter) {
    // If the snapshot changed, then we have to make sure we have the latest copy of the doc and
    // that it still matches.
    WorkingSetMember* member = ws->get(id);
    if (shard_role_details::getRecoveryUnit(opCtx)->getSnapshotId() != member->doc.snapshotId()) {
        std::unique_ptr<SeekableRecordCursor> cursor(collection->getCursor(opCtx));

        if (!WorkingSetCommon::fetch(opCtx, ws, id, cursor.get(), collection, collection->ns())) {
            // Doc is already deleted.
            return false;
        }
        ++docsFetchedCounter;

        // Make sure the re-fetched doc still matches the predicate.
        if (cq &&
            !exec::matcher::matchesBSON(
                cq->getPrimaryMatchExpression(), member->doc.value().toBson(), nullptr)) {
            // No longer matches.
            return false;
        }

        // Ensure that the BSONObj underlying the WorkingSetMember is owned because the cursor's
        // destructor is allowed to free the memory.
        member->makeObjOwnedIfNeeded();
    }
    return true;
}

bool isRetryableWrite(OperationContext* opCtx) {
    const auto replCoord{repl::ReplicationCoordinator::get(opCtx)};
    return replCoord->isRetryableWrite(opCtx);
}

}  // namespace write_stage_common
}  // namespace mongo
