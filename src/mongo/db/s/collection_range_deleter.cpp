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
                 "Collection sharding metadata destroyed"});
}

bool CollectionRangeDeleter::cleanUpNextRange(OperationContext* opCtx,
                                              NamespaceString const& nss,
                                              int maxToDelete,
                                              CollectionRangeDeleter* rangeDeleterForTestOnly) {
    StatusWith<int> wrote = 0;
    auto range = boost::optional<ChunkRange>(boost::none);
    {
        AutoGetCollection autoColl(opCtx, nss, MODE_IX);
        auto* collection = autoColl.getCollection();
        if (!collection) {
            return false;  // collection was dropped
        }

        auto* css = CollectionShardingState::get(opCtx, nss);
        {
            auto scopedCollectionMetadata = css->getMetadata();
            if (!scopedCollectionMetadata) {
                return false;  // collection was unsharded
            }

            // We don't actually know if this is the same collection that we were originally
            // scheduled to do deletions on, or another one with the same name. But it doesn't
            // matter: if it has deletions scheduled, now is as good a time as any to do them.
            auto self = rangeDeleterForTestOnly ? rangeDeleterForTestOnly
                                                : &css->_metadataManager._rangesToClean;
            {
                stdx::lock_guard<stdx::mutex> scopedLock(css->_metadataManager._managerLock);
                if (self->isEmpty())
                    return false;

                const auto& frontRange = self->_orphans.front().range;
                range.emplace(frontRange.getMin().getOwned(), frontRange.getMax().getOwned());
            }

            try {
                auto keyPattern = scopedCollectionMetadata->getKeyPattern();

                wrote = self->_doDeletion(opCtx, collection, keyPattern, *range, maxToDelete);
            } catch (const DBException& e) {
                wrote = e.toStatus();
                warning() << e.what();
            }

            if (!wrote.isOK() || wrote.getValue() == 0) {
                stdx::lock_guard<stdx::mutex> scopedLock(css->_metadataManager._managerLock);
                self->_pop(wrote.getStatus());
                return true;
            }
        }  // drop scopedCollectionMetadata
    }      // drop autoColl

    invariant(range);
    invariantOK(wrote.getStatus());
    invariant(wrote.getValue() > 0);

    log() << "Deleted " << wrote.getValue() << " documents in " << nss.ns() << " range " << *range;

    // Wait for replication outside the lock
    const auto clientOpTime = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
    WriteConcernResult unusedWCResult;
    Status status = Status::OK();
    try {
        status = waitForWriteConcern(opCtx, clientOpTime, kMajorityWriteConcern, &unusedWCResult);
    } catch (const DBException& e) {
        status = e.toStatus();
    }

    if (!status.isOK()) {
        warning() << "Error when waiting for write concern after removing " << nss << " range "
                  << *range << " : " << status.reason();
    }

    return true;
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
        {
            WriteUnitOfWork wuow(opCtx);
            if (saver) {
                saver->goingToDelete(obj);
            }
            collection->deleteDocument(opCtx, rloc, nullptr, true);

            wuow.commit();
        }
    } while (++numDeleted < maxToDelete);

    return numDeleted;
}

auto CollectionRangeDeleter::overlaps(ChunkRange const& range) const -> DeleteNotification {
    // start search with newest entries by using reverse iterators
    auto it = find_if(_orphans.rbegin(), _orphans.rend(), [&](auto& cleanee) {
        return bool(cleanee.range.overlapWith(range));
    });
    return it != _orphans.rend() ? it->notification : DeleteNotification();
}

void CollectionRangeDeleter::add(ChunkRange const& range) {
    // We ignore the case of overlapping, or even equal, ranges.
    // Deleting overlapping ranges is quick.
    _orphans.emplace_back(Deletion{ChunkRange(range.getMin().getOwned(), range.getMax().getOwned()),
                                   std::make_shared<Notification<Status>>()});
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
        if (*(range.notification)) {
            continue;  // was triggered in the test driver
        }
        range.notification->set(status);  // wake up anything still waiting
    }
    _orphans.clear();
}

void CollectionRangeDeleter::_pop(Status result) {
    _orphans.front().notification->set(result);  // wake up waitForClean
    _orphans.pop_front();
}

}  // namespace mongo
