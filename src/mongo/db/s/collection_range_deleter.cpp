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

#include "mongo/db/catalog/collection.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/query_knobs.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/metadata_manager.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/service_context.h"
#include "mongo/db/write_concern.h"
#include "mongo/executor/task_executor.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

class ChunkRange;

using CallbackArgs = executor::TaskExecutor::CallbackArgs;
using logger::LogComponent;

namespace {

const WriteConcernOptions kMajorityWriteConcern(WriteConcernOptions::kMajority,
                                                WriteConcernOptions::SyncMode::UNSET,
                                                Seconds(60));
}  // unnamed namespace

CollectionRangeDeleter::~CollectionRangeDeleter() {
    // notify anybody still sleeping on orphan ranges
    clear(Status{ErrorCodes::InterruptedDueToReplStateChange,
                 "Collection sharding metadata discarded"});
}

auto CollectionRangeDeleter::cleanUpNextRange(OperationContext* opCtx,
                                              NamespaceString const& nss,
                                              OID const& epoch,
                                              int maxToDelete,
                                              CollectionRangeDeleter* forTestOnly)
    -> boost::optional<Date_t> {

    StatusWith<int> wrote = 0;

    auto range = boost::optional<ChunkRange>(boost::none);
    auto notification = DeleteNotification();
    {
        AutoGetCollection autoColl(opCtx, nss, MODE_IX);
        auto* collection = autoColl.getCollection();
        auto* css = CollectionShardingState::get(opCtx, nss);
        {
            auto scopedCollectionMetadata = css->getMetadata();
            if (!forTestOnly && (!collection || !scopedCollectionMetadata)) {
                if (!collection) {
                    log() << "Abandoning any range deletions left over from dropped " << nss.ns();
                } else {
                    log() << "Abandoning any range deletions left over from previously sharded"
                          << nss.ns();
                }
                stdx::lock_guard<stdx::mutex> lk(css->_metadataManager->_managerLock);
                css->_metadataManager->_clearAllCleanups();
                return boost::none;
            }
            if (!forTestOnly && scopedCollectionMetadata->getCollVersion().epoch() != epoch) {
                LOG(1) << "Range deletion task for " << nss.ns() << " epoch " << epoch << " woke;"
                       << " (current is " << scopedCollectionMetadata->getCollVersion() << ")";
                return boost::none;
            }
            auto self = forTestOnly ? forTestOnly : &css->_metadataManager->_rangesToClean;
            bool writeOpLog = false;
            {
                stdx::lock_guard<stdx::mutex> scopedLock(css->_metadataManager->_managerLock);
                if (self->isEmpty()) {
                    LOG(1) << "No further range deletions scheduled on " << nss.ns();
                    return boost::none;
                }
                auto& o = self->_orphans;
                if (o.empty()) {
                    // We have delayed deletions; see if any are ready.
                    auto& df = self->_delayedOrphans.front();
                    if (df.whenToDelete > Date_t::now()) {
                        log() << "Deferring deletion of " << nss.ns() << " range "
                              << redact(df.range.toString()) << " until " << df.whenToDelete;
                        return df.whenToDelete;
                    }
                    // Move a single range from _delayedOrphans to _orphans:
                    o.splice(o.end(), self->_delayedOrphans, self->_delayedOrphans.begin());
                    LOG(1) << "Proceeding with deferred deletion of " << nss.ns() << " range "
                           << redact(o.front().range.toString());
                    writeOpLog = true;
                }
                invariant(!o.empty());
                const auto& frontRange = o.front().range;
                range.emplace(frontRange.getMin().getOwned(), frontRange.getMax().getOwned());
                notification = o.front().notification;
            }
            invariant(range);

            if (writeOpLog) {
                // clang-format off
                // Secondaries will watch for this update, and kill any queries that may depend on
                // documents in the range -- excepting any queries with a read-concern option
                // 'ignoreChunkMigration'
                try {
                    auto& serverConfigurationNss = NamespaceString::kServerConfigurationNamespace;
                    auto epoch = scopedCollectionMetadata->getCollVersion().epoch();
                    AutoGetCollection autoAdmin(opCtx, serverConfigurationNss, MODE_IX);

                    Helpers::upsert(opCtx, serverConfigurationNss.ns(),
                        BSON("_id" << "startRangeDeletion" << "ns" << nss.ns() << "epoch" << epoch
                          << "min" << range->getMin() << "max" << range->getMax()));

                } catch (DBException const& e) {
                    stdx::lock_guard<stdx::mutex> scopedLock(css->_metadataManager->_managerLock);
                    css->_metadataManager->_clearAllCleanups(
                        {ErrorCodes::fromInt(e.getCode()),
                         str::stream() << "cannot push startRangeDeletion record to Op Log,"
                                          " abandoning scheduled range deletions: " << e.what()});
                    return boost::none;
                }
                // clang-format on
            }

            try {
                auto keyPattern = scopedCollectionMetadata->getKeyPattern();

                wrote = self->_doDeletion(opCtx, collection, keyPattern, *range, maxToDelete);
            } catch (const DBException& e) {
                wrote = e.toStatus();
                warning() << e.what();
            }
            if (!wrote.isOK() || wrote.getValue() == 0) {
                if (wrote.isOK()) {
                    log() << "No documents remain to delete in " << nss << " range "
                          << redact(range->toString());
                }
                stdx::lock_guard<stdx::mutex> scopedLock(css->_metadataManager->_managerLock);
                self->_pop(wrote.getStatus());
                if (!self->_orphans.empty()) {
                    LOG(1) << "Deleting " << nss.ns() << " range "
                           << redact(self->_orphans.front().range.toString()) << " next.";
                }
                return Date_t{};
            }
        }  // drop scopedCollectionMetadata
    }      // drop autoColl

    invariant(range);
    invariantOK(wrote.getStatus());
    invariant(wrote.getValue() > 0);

    repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);
    const auto clientOpTime = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();

    // Wait for replication outside the lock
    WriteConcernResult unusedWCResult;
    Status status = Status::OK();
    try {
        status = waitForWriteConcern(opCtx, clientOpTime, kMajorityWriteConcern, &unusedWCResult);
    } catch (const DBException& e) {
        status = e.toStatus();
    }
    if (!status.isOK()) {
        log() << "Error when waiting for write concern after removing " << nss << " range "
              << redact(range->toString()) << " : " << redact(status.reason());

        AutoGetCollection autoColl(opCtx, nss, MODE_IX);
        auto* css = CollectionShardingState::get(opCtx, nss);
        stdx::lock_guard<stdx::mutex> scopedLock(css->_metadataManager->_managerLock);
        auto* self = &css->_metadataManager->_rangesToClean;
        // if range were already popped (e.g. by dropping nss during the waitForWriteConcern above)
        // its notification would have been triggered, so this check suffices to ensure that it is
        // safe to pop the range here.
        if (!notification.ready()) {
            invariant(!self->isEmpty() && self->_orphans.front().notification == notification);
            log() << "Abandoning deletion of latest range in " << nss.ns() << " after "
                  << wrote.getValue() << " local deletions because of replication failure";
            self->_pop(status);
        }
    } else {
        log() << "Deleted " << wrote.getValue() << " documents in " << nss.ns() << " range "
              << redact(range->toString());
    }

    notification.abandon();
    return Date_t{};
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
    if (idx == NULL) {
        std::string msg = str::stream() << "Unable to find shard key index for "
                                        << keyPattern.toString() << " in " << nss.ns();
        log() << msg;
        return {ErrorCodes::InternalError, msg};
    }

    // Extend bounds to match the index we found
    KeyPattern indexKeyPattern(idx->keyPattern().getOwned());
    auto extend = [&](auto& key) {
        return Helpers::toKeyFormat(indexKeyPattern.extendRangeBound(key, false));
    };
    const BSONObj& min = extend(range.getMin());
    const BSONObj& max = extend(range.getMax());

    LOG(1) << "begin removal of " << min << " to " << max << " in " << nss.ns();

    auto indexName = idx->indexName();
    IndexDescriptor* descriptor = collection->getIndexCatalog()->findIndexByName(opCtx, indexName);
    if (!descriptor) {
        std::string msg = str::stream() << "shard key index with name " << indexName << " on '"
                                        << nss.ns() << "' was dropped";
        log() << msg;
        return {ErrorCodes::InternalError, msg};
    }

    boost::optional<Helpers::RemoveSaver> saver;
    if (serverGlobalParams.moveParanoia) {
        saver.emplace("moveChunk", nss.ns(), "cleaning");
    }

    int numDeleted = 0;
    do {
        auto halfOpen = BoundInclusion::kIncludeStartKeyOnly;
        auto manual = PlanExecutor::YIELD_MANUAL;
        auto forward = InternalPlanner::FORWARD;
        auto fetch = InternalPlanner::IXSCAN_FETCH;

        auto exec = InternalPlanner::indexScan(
            opCtx, collection, descriptor, min, max, halfOpen, manual, forward, fetch);

        RecordId rloc;
        BSONObj obj;
        PlanExecutor::ExecState state = exec->getNext(&obj, &rloc);
        if (state == PlanExecutor::IS_EOF) {
            break;
        }
        if (state == PlanExecutor::FAILURE || state == PlanExecutor::DEAD) {
            warning(LogComponent::kSharding)
                << PlanExecutor::statestr(state) << " - cursor error while trying to delete " << min
                << " to " << max << " in " << nss << ": " << WorkingSetCommon::toStatusString(obj)
                << ", stats: " << Explain::getWinningPlanStats(exec.get());
            break;
        }
        invariant(PlanExecutor::ADVANCED == state);

        writeConflictRetry(opCtx, "delete range", nss.ns(), [&] {
            WriteUnitOfWork wuow(opCtx);
            if (saver) {
                saver->goingToDelete(obj).transitional_ignore();
            }
            collection->deleteDocument(opCtx, kUninitializedStmtId, rloc, nullptr, true);
            wuow.commit();
        });
    } while (++numDeleted < maxToDelete);

    return numDeleted;
}

namespace {

using Deletion = CollectionRangeDeleter::Deletion;
using DeleteNotification = CollectionRangeDeleter::DeleteNotification;
using Notif = boost::optional<DeleteNotification>;

auto checkOverlap(std::list<Deletion> const& deletions, ChunkRange const& range)
    -> boost::optional<DeleteNotification> {
    // start search with newest entries by using reverse iterators
    auto it = find_if(deletions.rbegin(), deletions.rend(), [&](auto& cleanee) {
        return bool(cleanee.range.overlapWith(range));
    });
    return (it != deletions.rend()) ? Notif{it->notification} : Notif{boost::none};
}

}  // namespace

auto CollectionRangeDeleter::overlaps(ChunkRange const& range) const
    -> boost::optional<DeleteNotification> {
    auto result = checkOverlap(_orphans, range);
    if (result) {
        return result;
    }
    return checkOverlap(_delayedOrphans, range);
}

auto CollectionRangeDeleter::add(std::list<Deletion> ranges) -> boost::optional<Date_t> {
    // We ignore the case of overlapping, or even equal, ranges.
    // Deleting overlapping ranges is quick.
    bool wasScheduledImmediate = !_orphans.empty();
    bool wasScheduledLater = !_delayedOrphans.empty();
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
    : notification(std::make_shared<Notification<Status>>()) {}

CollectionRangeDeleter::DeleteNotification::DeleteNotification(Status status)
    : notification(std::make_shared<Notification<Status>>()) {
    notify(status);
}

Status CollectionRangeDeleter::DeleteNotification::waitStatus(OperationContext* opCtx) {
    try {
        return notification->get(opCtx);
    } catch (...) {
        notification = std::make_shared<Notification<Status>>();
        notify({ErrorCodes::Interrupted, "Wait for range delete request completion interrupted"});
        throw;
    }
}

}  // namespace mongo
