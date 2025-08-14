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
#include "mongo/logv2/log.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log_and_backoff.h"

#include <algorithm>
#include <utility>

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {

bool KVDropPendingIdentReaper::IdentInfo::isExpired(const KVEngine* engine,
                                                    const Timestamp& ts) const {
    return identState == IdentInfo::State::kNotDropped && dropToken.expired() &&
        visit(OverloadedVisitor{[&](const Timestamp& dropTs) {
                                    return dropTs < ts || dropTs == Timestamp::min();
                                },
                                [&](const StorageEngine::CheckpointIteration& iteration) {
                                    return engine->hasDataBeenCheckpointed(iteration);
                                }},
              dropTime);
}

KVDropPendingIdentReaper::KVDropPendingIdentReaper(KVEngine* engine) : _engine(engine) {}

void KVDropPendingIdentReaper::addDropPendingIdent(
    const std::variant<Timestamp, StorageEngine::CheckpointIteration>& dropTime,
    std::shared_ptr<Ident> ident,
    StorageEngine::DropIdentCallback&& onDrop) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    auto dropTimestamp = visit(OverloadedVisitor{[](const Timestamp& ts) { return ts; },
                                                 [](const StorageEngine::CheckpointIteration&) {
                                                     return Timestamp::min();
                                                 }},
                               dropTime);
    const auto equalRange = _dropPendingIdents.equal_range(dropTimestamp);
    const auto& lowerBound = equalRange.first;
    const auto& upperBound = equalRange.second;
    auto matcher = [ident](const auto& pair) {
        return pair.second->identName == ident->getIdent();
    };
    if (std::find_if(lowerBound, upperBound, matcher) == upperBound) {
        auto info = std::make_shared<IdentInfo>();
        info->identName = ident->getIdent();
        info->identState = IdentInfo::State::kNotDropped;
        info->dropToken = ident;
        info->dropTime = dropTime;
        info->onDrop = std::move(onDrop);
        _dropPendingIdents.insert(std::make_pair(dropTimestamp, info));
        _identToTimestamp.insert(std::make_pair(ident->getIdent(), dropTimestamp));
    } else {
        LOGV2_WARNING(8097403,
                      "Ignoring duplicate ident drop with same drop time",
                      "ident"_attr = ident->getIdent(),
                      "dropTimestamp"_attr = dropTimestamp);
    }
}

std::shared_ptr<Ident> KVDropPendingIdentReaper::markIdentInUse(StringData ident) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    auto dropPendingIdent = _getDropPendingTSAndInfo(lock, ident);
    if (!dropPendingIdent) {
        return nullptr;
    }
    auto& [_, info] = *dropPendingIdent;

    if (info->identState == IdentInfo::State::kBeingDropped) {
        // The ident is being dropped or was already dropped. Cannot mark the ident as in use.
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
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    auto [it, end] = _dropPendingIdents.equal_range(Timestamp::min());
    if (end != _dropPendingIdents.end()) {
        // Include the earliest timestamped write as well.
        end++;
    }
    return std::any_of(it, end, [&](const auto& kv) { return kv.second->isExpired(_engine, ts); });
}


boost::optional<Timestamp> KVDropPendingIdentReaper::getEarliestDropTimestamp() const {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    auto it = _dropPendingIdents.cbegin();
    if (it == _dropPendingIdents.cend()) {
        return boost::none;
    }
    return it->first;
}

std::set<std::string> KVDropPendingIdentReaper::getAllIdentNames() const {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    std::set<std::string> identNames;
    for (const auto& [_, identInfo] : _dropPendingIdents) {
        const auto& identName = identInfo->identName;
        identNames.insert(identName);
    }
    return identNames;
}

size_t KVDropPendingIdentReaper::getNumIdents() const {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    return _dropPendingIdents.size();
};

void KVDropPendingIdentReaper::dropIdentsOlderThan(OperationContext* opCtx, const Timestamp& ts) {
    stdx::lock_guard lock(_dropMutex);

    std::vector<typename DropPendingIdents::value_type> toDrop;
    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        for (auto it = _dropPendingIdents.begin();
             it != _dropPendingIdents.end() && (it->first < ts || it->first == Timestamp::min());
             ++it) {
            // This collection/index satisfies the 'ts' requirement to be safe to drop, but we must
            // also check that there are no active operations remaining that still retain a
            // reference by which to access the collection/index data.
            const auto& info = it->second;
            if (info->isExpired(_engine, ts)) {
                info->identState = IdentInfo::State::kBeingDropped;
                toDrop.push_back(*it);
            }
        }
    }

    if (toDrop.empty()) {
        return;
    }

    for (auto& [dropTimestamp, identInfo] : toDrop) {
        // Dropping tables can be expensive since it involves disk operations. If the table also
        // needs a checkpoint, that adds even more overhead.
        opCtx->checkForInterrupt();

        auto status = _tryToDrop(lock, opCtx, dropTimestamp, *identInfo);
        if (status == ErrorCodes::ObjectIsBusy) {
            LOGV2_PROD_ONLY(6936300,
                            "Drop-pending ident is still in use",
                            "ident"_attr = identInfo->identName,
                            "dropTimestamp"_attr = dropTimestamp,
                            "error"_attr = status);
        } else if (!status.isOK()) {
            LOGV2_FATAL_NOTRACE(51022,
                                "Failed to remove drop-pending ident",
                                "ident"_attr = identInfo->identName,
                                "dropTimestamp"_attr = dropTimestamp,
                                "error"_attr = status);
        }
    }
}

void KVDropPendingIdentReaper::clearDropPendingState(OperationContext* opCtx) {
    invariant(shard_role_details::getLocker(opCtx)->isW());

    stdx::lock_guard dropLock(_dropMutex);
    stdx::lock_guard stateLock(_mutex);
    // We only delete the timestamped drops. Non-timestamped drops cannot be rolled back, and the
    // drops should still go through.
    auto firstElem = std::find_if_not(_dropPendingIdents.begin(),
                                      _dropPendingIdents.end(),
                                      [](const auto& kv) { return kv.first == Timestamp::min(); });
    _dropPendingIdents.erase(firstElem, _dropPendingIdents.end());
    absl::erase_if(_identToTimestamp,
                   [&](const auto& kv) { return kv.second != Timestamp::min(); });
}

Status KVDropPendingIdentReaper::immediatelyCompletePendingDrop(OperationContext* opCtx,
                                                                StringData ident) {
    // Acquiring _dropMutex is potentially expensive (it may involve waiting on IO being done on
    // another thread), so first check if the ident is known to the reaper without acquiring it.
    {
        stdx::lock_guard lock(_mutex);
        if (!_identToTimestamp.contains(ident))
            return Status::OK();
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
    auto tsAndInfo = [&] {
        stdx::lock_guard stateLock(_mutex);
        auto tsAndInfo = _getDropPendingTSAndInfo(stateLock, ident);
        if (tsAndInfo) {
            auto& info = *tsAndInfo->second;
            invariant(info.identState == IdentInfo::State::kNotDropped);
            invariant(info.dropToken.expired());
            info.identState = IdentInfo::State::kBeingDropped;
        }
        return tsAndInfo;
    }();

    if (!tsAndInfo) {
        // While we held no mutexes another thread completed the drop on this ident
        return Status::OK();
    }
    return _tryToDrop(dropLock, opCtx, tsAndInfo->first, *tsAndInfo->second);
}

boost::optional<std::pair<Timestamp, std::shared_ptr<KVDropPendingIdentReaper::IdentInfo>>>
KVDropPendingIdentReaper::_getDropPendingTSAndInfo(WithLock, StringData ident) const {
    const auto tsIter = _identToTimestamp.find(ident);
    if (tsIter == _identToTimestamp.end()) {
        // Ident is not known to the reaper.
        return boost::none;
    }

    auto ts = tsIter->second;
    const auto identRange = _dropPendingIdents.equal_range(ts);
    auto it = std::find_if(identRange.first, identRange.second, [&](const auto& entry) {
        return entry.second->identName == ident;
    });

    // The ident was found in '_identToTimestamp' earlier, so it must exist in '_dropPendingIdents'.
    invariant(it != identRange.second);
    return std::make_pair(ts, it->second);
}

Status KVDropPendingIdentReaper::_tryToDrop(WithLock,
                                            OperationContext* opCtx,
                                            Timestamp dropTimestamp,
                                            IdentInfo& identInfo) {
    LOGV2_PROD_ONLY(22237,
                    "Completing drop for ident",
                    "ident"_attr = identInfo.identName,
                    "dropTimestamp"_attr = dropTimestamp);

    // Ident drops are non-transactional and cannot be rolled back. So this does not
    // need to be in a WriteUnitOfWork.
    auto status = _engine->dropIdent(*shard_role_details::getRecoveryUnit(opCtx),
                                     identInfo.identName,
                                     ident::isCollectionIdent(identInfo.identName),
                                     identInfo.onDrop);
    if (!status.isOK()) {
        stdx::lock_guard lock(_mutex);
        identInfo.identState = IdentInfo::State::kNotDropped;
        return status;
    }

    LOGV2(6776600,
          "The ident was successfully dropped",
          "ident"_attr = identInfo.identName,
          "dropTimestamp"_attr = dropTimestamp);

    stdx::lock_guard<stdx::mutex> lock(_mutex);
    auto begin = _dropPendingIdents.lower_bound(dropTimestamp);
    for (auto it = begin; it != _dropPendingIdents.end() && it->first == dropTimestamp; ++it) {
        if (it->second.get() == &identInfo) {
            invariant(_identToTimestamp.erase(identInfo.identName) == 1);
            _dropPendingIdents.erase(it);
            return status;
        }
    }

    // If we get here then the ident was removed from _dropPendingIdents while we were dropping the
    // ident. The only way to remove idents without dropping them is clearDropPendingState(), and
    // since that is called specifically to prevent dropping idents it'd be a major problem if it's
    // called while we're in the middle of reaping.
    LOGV2_FATAL(10786001,
                "Did not find ident in _dropPendingIdents after dropping ident, indicating that "
                "illegal concurrent operations occurred",
                "ident"_attr = identInfo.identName);
}

}  // namespace mongo
