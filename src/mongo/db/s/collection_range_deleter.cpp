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
                                              Action action,
                                              int maxToDelete,
                                              CollectionRangeDeleter* forTestOnly) -> Action {

    invariant(action != Action::kFinished);
    StatusWith<int> wrote = 0;
    auto range = boost::optional<ChunkRange>(boost::none);
    auto notification = DeleteNotification();
    {
        AutoGetCollection autoColl(opCtx, nss, MODE_IX);
        auto* collection = autoColl.getCollection();
        auto* css = CollectionShardingState::get(opCtx, nss);
        {
            auto scopedCollectionMetadata = css->getMetadata();
            if ((!collection || !scopedCollectionMetadata) && !forTestOnly) {
                log() << "Abandoning range deletions left over from previously sharded collection"
                      << nss.ns();
                stdx::lock_guard<stdx::mutex> lk(css->_metadataManager->_managerLock);
                css->_metadataManager->_clearAllCleanups();
                return Action::kFinished;
            }

            // We don't actually know if this is the same collection that we were originally
            // scheduled to do deletions on, or another one with the same name. But it doesn't
            // matter: if it has a record of deletions scheduled, now is as good a time as any
            // to do them.

            auto self = forTestOnly ? forTestOnly : &css->_metadataManager->_rangesToClean;
            {
                stdx::lock_guard<stdx::mutex> scopedLock(css->_metadataManager->_managerLock);
                if (self->isEmpty())
                    return Action::kFinished;

                const auto& frontRange = self->_orphans.front().range;
                range.emplace(frontRange.getMin().getOwned(), frontRange.getMax().getOwned());
                notification = self->_orphans.front().notification;
            }
            invariant(range);

            if (action == Action::kWriteOpLog) {
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
                    return Action::kFinished;
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
                return Action::kWriteOpLog;
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
    return Action::kMore;
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

        MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
            WriteUnitOfWork wuow(opCtx);
            if (saver) {
                saver->goingToDelete(obj).transitional_ignore();
            }
            collection->deleteDocument(opCtx, rloc, nullptr, true);
            wuow.commit();
        }
        MONGO_WRITE_CONFLICT_RETRY_LOOP_END(opCtx, "delete range", nss.ns());

    } while (++numDeleted < maxToDelete);

    return numDeleted;
}

auto CollectionRangeDeleter::overlaps(ChunkRange const& range) const
    -> boost::optional<DeleteNotification> {
    // start search with newest entries by using reverse iterators
    auto it = find_if(_orphans.rbegin(), _orphans.rend(), [&](auto& cleanee) {
        return bool(cleanee.range.overlapWith(range));
    });
    if (it == _orphans.rend()) {
        return boost::none;
    }
    return it->notification;
}

bool CollectionRangeDeleter::add(std::list<Deletion> ranges) {
    // We ignore the case of overlapping, or even equal, ranges.
    // Deleting overlapping ranges is quick.
    bool wasEmpty = _orphans.empty();
    _orphans.splice(_orphans.end(), ranges);
    return wasEmpty && !_orphans.empty();
}

void CollectionRangeDeleter::append(BSONObjBuilder* builder) const {
    BSONArrayBuilder arr(builder->subarrayStart("rangesToClean"));
    for (auto const& entry : _orphans) {
        BSONObjBuilder obj;
        entry.range.append(&obj);
        arr.append(obj.done());
    }
    arr.done();
}

size_t CollectionRangeDeleter::size() const {
    return _orphans.size();
}

bool CollectionRangeDeleter::isEmpty() const {
    return _orphans.empty();
}

void CollectionRangeDeleter::clear(Status status) {
    for (auto& range : _orphans) {
        range.notification.notify(status);  // wake up anything still waiting
    }
    _orphans.clear();
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
