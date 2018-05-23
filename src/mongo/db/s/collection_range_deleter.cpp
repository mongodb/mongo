/**
 *    Copyright (C) 2016 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/collection_range_deleter.h"

#include <algorithm>
#include <utility>

#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/query_knobs.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/service_context.h"
#include "mongo/db/write_concern.h"
#include "mongo/executor/task_executor.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

MONGO_EXPORT_SERVER_PARAMETER(rangeDeleterBatchDelayMS, int, 20)
    ->withValidator([](const int& newVal) {
        if (newVal < 0) {
            return Status(ErrorCodes::BadValue, "rangeDeleterBatchDelayMS must not be negative");
        }
        return Status::OK();
    });

namespace {

using Deletion = CollectionRangeDeleter::Deletion;
using DeleteNotification = CollectionRangeDeleter::DeleteNotification;

const WriteConcernOptions kMajorityWriteConcern(WriteConcernOptions::kMajority,
                                                WriteConcernOptions::SyncMode::UNSET,
                                                Seconds(60));

boost::optional<DeleteNotification> checkOverlap(std::list<Deletion> const& deletions,
                                                 ChunkRange const& range) {
    // Start search with newest entries by using reverse iterators
    auto it = find_if(deletions.rbegin(), deletions.rend(), [&](auto& cleanee) {
        return bool(cleanee.range.overlapWith(range));
    });

    if (it != deletions.rend())
        return it->notification;

    return boost::none;
}

}  // namespace

CollectionRangeDeleter::CollectionRangeDeleter() = default;

CollectionRangeDeleter::~CollectionRangeDeleter() {
    // Notify anybody still sleeping on orphan ranges
    clear({ErrorCodes::InterruptedDueToStepDown, "Collection sharding metadata discarded"});
}

boost::optional<Date_t> CollectionRangeDeleter::cleanUpNextRange(
    OperationContext* opCtx,
    NamespaceString const& nss,
    OID const& epoch,
    int maxToDelete,
    CollectionRangeDeleter* forTestOnly) {

    StatusWith<int> wrote = 0;

    auto range = boost::optional<ChunkRange>(boost::none);
    auto notification = DeleteNotification();

    {
        UninterruptibleLockGuard noInterrupt(opCtx->lockState());
        AutoGetCollection autoColl(opCtx, nss, MODE_IX);

        auto* const collection = autoColl.getCollection();
        auto* const css = CollectionShardingState::get(opCtx, nss);
        auto* const self = forTestOnly ? forTestOnly : &css->_metadataManager->_rangesToClean;

        auto scopedCollectionMetadata = css->getMetadata(opCtx);

        if (!forTestOnly && (!collection || !scopedCollectionMetadata)) {
            if (!collection) {
                LOG(0) << "Abandoning any range deletions left over from dropped " << nss.ns();
            } else {
                LOG(0) << "Abandoning any range deletions left over from previously sharded"
                       << nss.ns();
            }

            stdx::lock_guard<stdx::mutex> lk(css->_metadataManager->_managerLock);
            css->_metadataManager->_clearAllCleanups(lk);
            return boost::none;
        }

        if (!forTestOnly && scopedCollectionMetadata->getCollVersion().epoch() != epoch) {
            LOG(1) << "Range deletion task for " << nss.ns() << " epoch " << epoch << " woke;"
                   << " (current is " << scopedCollectionMetadata->getCollVersion() << ")";
            return boost::none;
        }

        bool writeOpLog = false;

        {
            stdx::lock_guard<stdx::mutex> scopedLock(css->_metadataManager->_managerLock);
            if (self->isEmpty()) {
                LOG(1) << "No further range deletions scheduled on " << nss.ns();
                return boost::none;
            }

            auto& orphans = self->_orphans;
            if (orphans.empty()) {
                // We have delayed deletions; see if any are ready.
                auto& df = self->_delayedOrphans.front();
                if (df.whenToDelete > Date_t::now()) {
                    LOG(0) << "Deferring deletion of " << nss.ns() << " range "
                           << redact(df.range.toString()) << " until " << df.whenToDelete;
                    return df.whenToDelete;
                }

                // Move a single range from _delayedOrphans to _orphans
                orphans.splice(orphans.end(), self->_delayedOrphans, self->_delayedOrphans.begin());
                LOG(1) << "Proceeding with deferred deletion of " << nss.ns() << " range "
                       << redact(orphans.front().range.toString());

                writeOpLog = true;
            }

            invariant(!orphans.empty());
            const auto& frontRange = orphans.front().range;
            range.emplace(frontRange.getMin().getOwned(), frontRange.getMax().getOwned());
            notification = orphans.front().notification;
        }

        invariant(range);

        if (writeOpLog) {
            // Secondaries will watch for this update, and kill any queries that may depend on
            // documents in the range -- excepting any queries with a read-concern option
            // 'ignoreChunkMigration'
            try {
                AutoGetCollection autoAdmin(
                    opCtx, NamespaceString::kServerConfigurationNamespace, MODE_IX);

                Helpers::upsert(opCtx,
                                NamespaceString::kServerConfigurationNamespace.ns(),
                                BSON("_id"
                                     << "startRangeDeletion"
                                     << "ns"
                                     << nss.ns()
                                     << "epoch"
                                     << epoch
                                     << "min"
                                     << range->getMin()
                                     << "max"
                                     << range->getMax()));
            } catch (const DBException& e) {
                stdx::lock_guard<stdx::mutex> scopedLock(css->_metadataManager->_managerLock);
                css->_metadataManager->_clearAllCleanups(
                    scopedLock,
                    e.toStatus("cannot push startRangeDeletion record to Op Log,"
                               " abandoning scheduled range deletions"));
                return boost::none;
            }
        }

        try {
            const auto keyPattern = scopedCollectionMetadata->getKeyPattern();
            wrote = self->_doDeletion(opCtx, collection, keyPattern, *range, maxToDelete);
        } catch (const DBException& e) {
            wrote = e.toStatus();
            warning() << e.what();
        }
    }  // drop autoColl

    if (!wrote.isOK() || wrote.getValue() == 0) {
        if (wrote.isOK()) {
            LOG(0) << "No documents remain to delete in " << nss << " range "
                   << redact(range->toString());
        }

        // Wait for majority replication even when wrote isn't OK or == 0, because it might have
        // been OK and/or > 0 previously, and the deletions must be persistent before notifying
        // clients in _pop().

        LOG(0) << "Waiting for majority replication of local deletions in " << nss.ns() << " range "
               << redact(range->toString());

        repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);
        const auto clientOpTime = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();

        // Wait for replication outside the lock
        const auto status = [&] {
            try {
                WriteConcernResult unusedWCResult;
                return waitForWriteConcern(
                    opCtx, clientOpTime, kMajorityWriteConcern, &unusedWCResult);
            } catch (const DBException& e) {
                return e.toStatus();
            }
        }();

        // Get the lock again to finish off this range (including notifying, if necessary).
        // Don't allow lock interrupts while cleaning up.
        UninterruptibleLockGuard noInterrupt(opCtx->lockState());
        AutoGetCollection autoColl(opCtx, nss, MODE_IX);
        auto* const css = CollectionShardingState::get(opCtx, nss);
        auto* const self = forTestOnly ? forTestOnly : &css->_metadataManager->_rangesToClean;
        stdx::lock_guard<stdx::mutex> scopedLock(css->_metadataManager->_managerLock);

        if (!status.isOK()) {
            LOG(0) << "Error when waiting for write concern after removing " << nss << " range "
                   << redact(range->toString()) << " : " << redact(status.reason());

            // If range were already popped (e.g. by dropping nss during the waitForWriteConcern
            // above) its notification would have been triggered, so this check suffices to ensure
            // that it is safe to pop the range here
            if (!notification.ready()) {
                invariant(!self->isEmpty() && self->_orphans.front().notification == notification);
                LOG(0) << "Abandoning deletion of latest range in " << nss.ns() << " after local "
                       << "deletions because of replication failure";
                self->_pop(status);
            }
        } else {
            LOG(0) << "Finished deleting documents in " << nss.ns() << " range "
                   << redact(range->toString());

            self->_pop(wrote.getStatus());
        }

        if (!self->_orphans.empty()) {
            LOG(1) << "Deleting " << nss.ns() << " range "
                   << redact(self->_orphans.front().range.toString()) << " next.";
        }

        return Date_t::now() + stdx::chrono::milliseconds{rangeDeleterBatchDelayMS.load()};
    }

    invariant(range);
    invariant(wrote.getStatus());
    invariant(wrote.getValue() > 0);

    notification.abandon();
    return Date_t::now() + stdx::chrono::milliseconds{rangeDeleterBatchDelayMS.load()};
}

StatusWith<int> CollectionRangeDeleter::_doDeletion(OperationContext* opCtx,
                                                    Collection* collection,
                                                    BSONObj const& keyPattern,
                                                    ChunkRange const& range,
                                                    int maxToDelete) {
    invariant(collection != nullptr);
    invariant(!isEmpty());

    auto const& nss = collection->ns();

    // The IndexChunk has a keyPattern that may apply to more than one index - we need to
    // select the index and get the full index keyPattern here.
    auto catalog = collection->getIndexCatalog();
    const IndexDescriptor* idx = catalog->findShardKeyPrefixedIndex(opCtx, keyPattern, false);
    if (!idx) {
        std::string msg = str::stream() << "Unable to find shard key index for "
                                        << keyPattern.toString() << " in " << nss.ns();
        LOG(0) << msg;
        return {ErrorCodes::InternalError, msg};
    }

    // Extend bounds to match the index we found
    const KeyPattern indexKeyPattern(idx->keyPattern());
    const auto extend = [&](const auto& key) {
        return Helpers::toKeyFormat(indexKeyPattern.extendRangeBound(key, false));
    };

    const auto min = extend(range.getMin());
    const auto max = extend(range.getMax());

    LOG(1) << "begin removal of " << min << " to " << max << " in " << nss.ns();

    const auto indexName = idx->indexName();
    IndexDescriptor* descriptor = collection->getIndexCatalog()->findIndexByName(opCtx, indexName);
    if (!descriptor) {
        std::string msg = str::stream() << "shard key index with name " << indexName << " on '"
                                        << nss.ns() << "' was dropped";
        LOG(0) << msg;
        return {ErrorCodes::InternalError, msg};
    }

    boost::optional<Helpers::RemoveSaver> saver;
    if (serverGlobalParams.moveParanoia) {
        saver.emplace("moveChunk", nss.ns(), "cleaning");
    }

    auto halfOpen = BoundInclusion::kIncludeStartKeyOnly;
    auto manual = PlanExecutor::YIELD_MANUAL;
    auto forward = InternalPlanner::FORWARD;
    auto fetch = InternalPlanner::IXSCAN_FETCH;

    auto exec = InternalPlanner::indexScan(
        opCtx, collection, descriptor, min, max, halfOpen, manual, forward, fetch);

    int numDeleted = 0;
    do {
        RecordId rloc;
        BSONObj obj;
        PlanExecutor::ExecState state = exec->getNext(&obj, &rloc);
        if (state == PlanExecutor::IS_EOF) {
            break;
        }
        if (state == PlanExecutor::FAILURE || state == PlanExecutor::DEAD) {
            warning() << PlanExecutor::statestr(state) << " - cursor error while trying to delete "
                      << redact(min) << " to " << redact(max) << " in " << nss << ": "
                      << redact(WorkingSetCommon::toStatusString(obj))
                      << ", stats: " << Explain::getWinningPlanStats(exec.get());
            break;
        }
        invariant(PlanExecutor::ADVANCED == state);

        exec->saveState();
        writeConflictRetry(opCtx, "delete range", nss.ns(), [&] {
            WriteUnitOfWork wuow(opCtx);
            if (saver) {
                uassertStatusOK(saver->goingToDelete(obj));
            }
            collection->deleteDocument(opCtx, kUninitializedStmtId, rloc, nullptr, true);
            wuow.commit();
        });
        auto restoreStateStatus = exec->restoreState();
        if (!restoreStateStatus.isOK()) {
            warning() << "error restoring cursor state while trying to delete " << redact(min)
                      << " to " << redact(max) << " in " << nss
                      << ", stats: " << Explain::getWinningPlanStats(exec.get()) << ": "
                      << redact(restoreStateStatus);
            break;
        }

    } while (++numDeleted < maxToDelete);

    return numDeleted;
}

auto CollectionRangeDeleter::overlaps(ChunkRange const& range) const
    -> boost::optional<DeleteNotification> {
    auto result = checkOverlap(_orphans, range);
    if (result) {
        return result;
    }
    return checkOverlap(_delayedOrphans, range);
}

boost::optional<Date_t> CollectionRangeDeleter::add(std::list<Deletion> ranges) {
    // We ignore the case of overlapping, or even equal, ranges. Deleting overlapping ranges is
    // quick.
    const bool wasScheduledImmediate = !_orphans.empty();
    const bool wasScheduledLater = !_delayedOrphans.empty();

    while (!ranges.empty()) {
        if (ranges.front().whenToDelete != Date_t{}) {
            _delayedOrphans.splice(_delayedOrphans.end(), ranges, ranges.begin());
        } else {
            _orphans.splice(_orphans.end(), ranges, ranges.begin());
        }
    }

    if (wasScheduledImmediate) {
        return boost::none;  // already scheduled
    } else if (!_orphans.empty()) {
        return Date_t{};
    } else if (wasScheduledLater) {
        return boost::none;  // already scheduled
    } else if (!_delayedOrphans.empty()) {
        return _delayedOrphans.front().whenToDelete;
    }

    return boost::none;
}

void CollectionRangeDeleter::append(BSONObjBuilder* builder) const {
    BSONArrayBuilder arr(builder->subarrayStart("rangesToClean"));
    for (auto const& entry : _orphans) {
        BSONObjBuilder obj;
        entry.range.append(&obj);
        arr.append(obj.done());
    }
    for (auto const& entry : _delayedOrphans) {
        BSONObjBuilder obj;
        entry.range.append(&obj);
        arr.append(obj.done());
    }
    arr.done();
}

size_t CollectionRangeDeleter::size() const {
    return _orphans.size() + _delayedOrphans.size();
}

bool CollectionRangeDeleter::isEmpty() const {
    return _orphans.empty() && _delayedOrphans.empty();
}

void CollectionRangeDeleter::clear(Status status) {
    for (auto& range : _orphans) {
        range.notification.notify(status);  // wake up anything still waiting
    }
    _orphans.clear();
    for (auto& range : _delayedOrphans) {
        range.notification.notify(status);  // wake up anything still waiting
    }
    _delayedOrphans.clear();
}

void CollectionRangeDeleter::_pop(Status result) {
    _orphans.front().notification.notify(result);  // wake up waitForClean
    _orphans.pop_front();
}

// DeleteNotification

CollectionRangeDeleter::DeleteNotification::DeleteNotification()
    : _notification(std::make_shared<Notification<Status>>()) {}

CollectionRangeDeleter::DeleteNotification::DeleteNotification(Status status)
    : _notification(std::make_shared<Notification<Status>>(std::move(status))) {}

Status CollectionRangeDeleter::DeleteNotification::waitStatus(OperationContext* opCtx) {
    try {
        return _notification->get(opCtx);
    } catch (const DBException& ex) {
        _notification = std::make_shared<Notification<Status>>(ex.toStatus());
        throw;
    }
}

}  // namespace mongo
