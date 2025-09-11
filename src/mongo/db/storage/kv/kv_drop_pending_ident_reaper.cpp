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


#include "mongo/db/storage/kv/kv_drop_pending_ident_reaper.h"

#include "mongo/bson/timestamp.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/ident.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/logv2/attribute_storage.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log_and_backoff.h"

#include <algorithm>
#include <compare>
#include <utility>
#include <variant>

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {
bool KVDropPendingIdentReaper::CompareByDropTime::operator()(
    const std::shared_ptr<IdentInfo>& a, const std::shared_ptr<IdentInfo>& b) const {
    return a->dropTime < b->dropTime;
}
bool KVDropPendingIdentReaper::CompareByDropTime::operator()(
    const std::shared_ptr<IdentInfo>& a, const StorageEngine::DropTime& b) const {
    return a->dropTime < b;
}
bool KVDropPendingIdentReaper::CompareByDropTime::operator()(
    const StorageEngine::DropTime& a, const std::shared_ptr<IdentInfo>& b) const {
    return a < b->dropTime;
}

bool KVDropPendingIdentReaper::IdentInfo::isExpired(const KVEngine* engine,
                                                    const Timestamp& ts) const {
    return !dropInProgress && dropToken.expired() &&
        visit(OverloadedVisitor{
                  [&](Timestamp dropTs) { return dropTs < ts || dropTs == Timestamp::min(); },
                  [&](StorageEngine::CheckpointIteration iteration) {
                      return engine->hasDataBeenCheckpointed(iteration);
                  }},
              dropTime);
}

KVDropPendingIdentReaper::KVDropPendingIdentReaper(KVEngine* engine) : _engine(engine) {}

void KVDropPendingIdentReaper::addDropPendingIdent(const StorageEngine::DropTime& dropTime,
                                                   std::shared_ptr<Ident> ident,
                                                   StorageEngine::DropIdentCallback&& onDrop) {
    stdx::lock_guard lock(_mutex);

    // Many tests drop an ident while a TemporaryRecordStore for that ident is alive, resulting in a
    // second drop when the TRS is destroyed. Allow that specific use, but otherwise we should not
    // see idents dropped while they're already drop-pending.
    if (_dropPendingIdents.contains(ident->getIdent())) {
        invariant(dropTime == Timestamp::min(), ident->getIdent());
        return;
    }

    auto info = std::make_shared<IdentInfo>();
    info->identName = ident->getIdent();
    info->dropToken = ident;
    info->dropTime = dropTime;
    info->onDrop = onDrop;
    _timestampOrderedIdents.insert(info);
    _dropPendingIdents.insert(std::make_pair(ident->getIdent(), info));
}

void KVDropPendingIdentReaper::dropUnknownIdent(const Timestamp& stableTimestamp,
                                                StringData ident) {
    stdx::lock_guard lock(_mutex);

    // There may already be drop-pending idents when we reload the catalog and drop all idents not
    // present at the stable timestamp. If the existing drop is untimestamped or before the stable
    // timestamp we should keep the existing timestamp. If it's after the stable timestamp, that
    // means the ident must also have *created* after the stable timestamp, as that's the only way
    // for an ident which has not yet been dropped at a timestamp to not exist at that timestamp.
    // This means that we've rolled back the creation of the ident, and should convert the drop to
    // an untimestamped drop.
    if (auto it = _dropPendingIdents.find(ident); it != _dropPendingIdents.end()) {
        if (it->second->dropTime <= stableTimestamp) {
            return;
        }

        auto info = it->second;
        _timestampOrderedIdents.erase(info);
        info->dropTime = Timestamp::min();
        _timestampOrderedIdents.insert(info);
        return;
    }

    auto info = std::make_shared<IdentInfo>();
    info->identName = std::string(ident);
    info->dropTime = stableTimestamp;
    info->dropTimeIsExact = false;
    _timestampOrderedIdents.insert(info);
    _dropPendingIdents.emplace(ident, std::move(info));
}

std::shared_ptr<Ident> KVDropPendingIdentReaper::markIdentInUse(StringData ident) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    auto it = _dropPendingIdents.find(ident);
    if (it == _dropPendingIdents.end()) {
        return nullptr;
    }

    auto& info = it->second;

    if (info->dropInProgress) {
        // The ident is being dropped and it's too late to mark the ident as in use.
        return nullptr;
    }

    if (auto existingIdent = info->dropToken.lock()) {
        // This function can be called concurrently and we need to share the same ident at any
        // given time to prevent the reaper from removing idents prematurely.
        return existingIdent;
    }

    std::shared_ptr<Ident> newIdent = std::make_shared<Ident>(info->identName);
    info->dropToken = newIdent;
    return newIdent;
}

bool KVDropPendingIdentReaper::hasExpiredIdents(const Timestamp& ts) const {
    stdx::lock_guard lock(_mutex);
    for (auto& info : _timestampOrderedIdents) {
        if (info->isExpired(_engine, ts))
            return true;
        if (info->dropTime > ts)
            return false;
    }
    return false;
}

boost::optional<Timestamp> KVDropPendingIdentReaper::getEarliestDropTimestamp() const {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    auto it = _timestampOrderedIdents.cbegin();
    if (it == _timestampOrderedIdents.cend()) {
        return boost::none;
    }
    return std::visit(OverloadedVisitor{[](Timestamp dropTs) { return dropTs; },
                                        [](StorageEngine::CheckpointIteration) {
                                            return Timestamp::min();
                                        }},
                      (*it)->dropTime);
}

std::vector<std::string> KVDropPendingIdentReaper::getAllIdentNames() const {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    std::vector<std::string> identNames;
    for (const auto& identInfo : _timestampOrderedIdents) {
        identNames.push_back(identInfo->identName);
    }
    return identNames;
}

size_t KVDropPendingIdentReaper::getNumIdents() const {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    return _timestampOrderedIdents.size();
};

void KVDropPendingIdentReaper::dropIdentsOlderThan(OperationContext* opCtx, const Timestamp& ts) {
    stdx::lock_guard lock(_dropMutex);

    std::vector<std::shared_ptr<IdentInfo>> toDrop;
    {
        stdx::lock_guard lock(_mutex);
        for (auto& info : _timestampOrderedIdents) {
            if (info->dropTime >= ts && ts > Timestamp::min())
                break;
            // This collection/index satisfies the 'ts' requirement to be safe to drop, but we must
            // also check that there are no active operations remaining that still retain a
            // reference by which to access the collection/index data.
            if (info->isExpired(_engine, ts)) {
                info->dropInProgress = true;
                toDrop.push_back(info);
            }
        }
    }

    if (toDrop.empty()) {
        return;
    }

    for (auto& identInfo : toDrop) {
        // Dropping tables can be expensive since it involves disk operations. If the table also
        // needs a checkpoint, that adds even more overhead.
        opCtx->checkForInterrupt();

        auto status = _tryToDrop(lock, opCtx, *identInfo);
        if (status == ErrorCodes::ObjectIsBusy) {
            LOGV2_PROD_ONLY(6936300,
                            "Drop-pending ident is still in use",
                            "ident"_attr = identInfo->identName,
                            "dropTimestamp"_attr = identInfo->dropTime,
                            "error"_attr = status);
        } else if (!status.isOK()) {
            LOGV2_FATAL_NOTRACE(51022,
                                "Failed to remove drop-pending ident",
                                "ident"_attr = identInfo->identName,
                                "dropTimestamp"_attr = identInfo->dropTime,
                                "error"_attr = status);
        }
    }
}

void KVDropPendingIdentReaper::rollbackDropsAfterStableTimestamp(Timestamp stableTimestamp) {
    stdx::lock_guard dropLock(_dropMutex);
    stdx::lock_guard stateLock(_mutex);
    auto firstElem = _timestampOrderedIdents.upper_bound(stableTimestamp);
    _timestampOrderedIdents.erase(firstElem, _timestampOrderedIdents.end());
    absl::erase_if(_dropPendingIdents,
                   [&](const auto& kv) { return kv.second->dropTime > stableTimestamp; });
    invariant(_timestampOrderedIdents.size() == _dropPendingIdents.size());
}

Status KVDropPendingIdentReaper::immediatelyCompletePendingDrop(OperationContext* opCtx,
                                                                StringData ident) {
    // Acquiring _dropMutex is potentially expensive (it may involve waiting on IO being done on
    // another thread), so first check if the ident is known to the reaper without acquiring it.
    {
        stdx::lock_guard lock(_mutex);
        auto it = _dropPendingIdents.find(ident);
        if (it == _dropPendingIdents.end())
            return Status::OK();
        if (it->second->dropTime > Timestamp::min() && it->second->dropTimeIsExact)
            return Status(ErrorCodes::ObjectIsBusy,
                          "Pending drop is timestamped so ident may still be in use");
    }

    for (size_t retries = 1;; ++retries) {
        auto status = _immediatelyAttemptToCompletePendingDrop(opCtx, ident);
        if (status != ErrorCodes::ObjectIsBusy) {
            return status;
        }

        if (auto interruptStatus = opCtx->checkForInterruptNoAssert(); !interruptStatus.isOK()) {
            return interruptStatus;
        }

        // This function is called on idents whose creation was rolled back. Despite that, there
        // could still be a transient reader on another thread due to the checkpointer touching
        // every table known to the storage engine. The checkpointer doesn't expose any hooks to
        // wait for it to complete, so just retry.
        logAndBackoff(10786000,
                      logv2::LogComponent::kStorage,
                      logv2::LogSeverity::Log(),
                      retries,
                      "Retrying immediate drop of drop-pending ident",
                      "error"_attr = status);
    }
}

Status KVDropPendingIdentReaper::_immediatelyAttemptToCompletePendingDrop(OperationContext* opCtx,
                                                                          StringData ident) {
    stdx::lock_guard dropLock(_dropMutex);
    auto info = [&]() -> DropPendingIdents::value_type {
        stdx::lock_guard stateLock(_mutex);
        auto it = _dropPendingIdents.find(ident);
        if (it == _dropPendingIdents.end()) {
            return nullptr;
        }
        auto& info = *it->second;
        invariant(!info.dropInProgress);
        invariant(info.dropToken.expired());
        info.dropInProgress = true;
        return it->second;
    }();

    if (!info) {
        // While we held no mutexes another thread completed the drop on this ident
        return Status::OK();
    }
    return _tryToDrop(dropLock, opCtx, *info);
}

Status KVDropPendingIdentReaper::_tryToDrop(WithLock,
                                            OperationContext* opCtx,
                                            IdentInfo& identInfo) {
    LOGV2_PROD_ONLY(22237,
                    "Completing drop for ident",
                    "ident"_attr = identInfo.identName,
                    "dropTimestamp"_attr = identInfo.dropTime);

    // Ident drops are non-transactional and cannot be rolled back. So this does not
    // need to be in a WriteUnitOfWork.
    auto status = _engine->dropIdent(*shard_role_details::getRecoveryUnit(opCtx),
                                     identInfo.identName,
                                     ident::isCollectionIdent(identInfo.identName),
                                     identInfo.onDrop);
    if (!status.isOK()) {
        stdx::lock_guard lock(_mutex);
        identInfo.dropInProgress = false;
        return status;
    }

    LOGV2(6776600,
          "The ident was successfully dropped",
          "ident"_attr = identInfo.identName,
          "dropTimestamp"_attr = identInfo.dropTime);

    stdx::lock_guard<stdx::mutex> lock(_mutex);
    auto [begin, end] = _timestampOrderedIdents.equal_range(identInfo.dropTime);
    for (auto it = begin; it != end; ++it) {
        if (it->get() == &identInfo) {
            invariant(_dropPendingIdents.erase(identInfo.identName) == 1);
            _timestampOrderedIdents.erase(it);
            return status;
        }
    }

    // If we get here then the ident was removed from _timestampOrderedIdents while we were dropping
    // the ident. The only way to remove idents without dropping them is
    // rollbackDropsAfterStableTimestamp(), and since that is called specifically to prevent
    // dropping idents it'd be a major problem if it's called while we're in the middle of reaping.
    LOGV2_FATAL(
        10786001,
        "Did not find ident in _timestampOrderedIdents after dropping ident, indicating that "
        "illegal concurrent operations occurred",
        "ident"_attr = identInfo.identName);
}

}  // namespace mongo
