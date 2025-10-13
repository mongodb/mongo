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

#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/storage/ident.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"

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
    DropPendingIdents toDrop;
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
                toDrop.insert(*it);
            }
        }
    }

    if (toDrop.empty()) {
        return;
    }

    for (auto& timestampAndIdentInfo : toDrop) {
        // Dropping tables can be expensive since it involves disk operations. If the table also
        // needs a checkpoint, that adds even more overhead.
        opCtx->checkForInterrupt();

        const auto& dropTimestamp = timestampAndIdentInfo.first;
        auto& identInfo = timestampAndIdentInfo.second;
        const auto& identName = identInfo->identName;
        LOGV2_PROD_ONLY(22237,
                        "Completing drop for ident",
                        "ident"_attr = identName,
                        "dropTimestamp"_attr = dropTimestamp);
        // Ident drops are non-transactional and cannot be rolled back. So this does not
        // need to be in a WriteUnitOfWork.
        auto status = _engine->dropIdent(*shard_role_details::getRecoveryUnit(opCtx),
                                         identName,
                                         ident::isCollectionIdent(identName),
                                         identInfo->onDrop);
        if (!status.isOK()) {
            if (status == ErrorCodes::ObjectIsBusy) {
                LOGV2_PROD_ONLY(6936300,
                                "Drop-pending ident is still in use",
                                "ident"_attr = identName,
                                "dropTimestamp"_attr = dropTimestamp,
                                "error"_attr = status);

                stdx::lock_guard<stdx::mutex> lock(_mutex);
                identInfo->identState = IdentInfo::State::kNotDropped;
                _identStateResetOrRemovedCV.notify_all();
                continue;
            }
            LOGV2_FATAL_NOTRACE(51022,
                                "Failed to remove drop-pending ident",
                                "ident"_attr = identName,
                                "dropTimestamp"_attr = dropTimestamp,
                                "error"_attr = status);
        }

        {
            stdx::lock_guard<stdx::mutex> lock(_mutex);
            _stopTrackingIdentAndNotify(lock, dropTimestamp, identInfo);
        }

        LOGV2(6776600,
              "The ident was successfully dropped",
              "ident"_attr = identName,
              "dropTimestamp"_attr = dropTimestamp);
    }
}

void KVDropPendingIdentReaper::clearDropPendingState(OperationContext* opCtx) {
    invariant(shard_role_details::getLocker(opCtx)->isW());

    stdx::lock_guard<stdx::mutex> lock(_mutex);
    // We only delete the timestamped drops. Non-timestamped drops cannot be rolled back, and the
    // drops should still go through.
    auto firstElem = std::find_if_not(_dropPendingIdents.begin(),
                                      _dropPendingIdents.end(),
                                      [](const auto& kv) { return kv.first == Timestamp::min(); });
    _dropPendingIdents.erase(firstElem, _dropPendingIdents.end());
    absl::erase_if(_identToTimestamp,
                   [&](const auto& kv) { return kv.second != Timestamp::min(); });
    _identStateResetOrRemovedCV.notify_all();
}

void KVDropPendingIdentReaper::clearDropPendingStateForIdent(OperationContext* opCtx,
                                                             StringData ident) {
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    opCtx->waitForConditionOrInterrupt(_identStateResetOrRemovedCV, lock, [&] {
        auto tsAndInfo = _getDropPendingTSAndInfo(lock, ident);
        if (!tsAndInfo) {
            return true;
        }
        auto& [timestamp, identInfo] = *tsAndInfo;
        if (identInfo->identState != IdentInfo::State::kBeingDropped) {
            _stopTrackingIdentAndNotify(lock, timestamp, identInfo);
        }

        // 'kBeingDropped' means the reaper is actively removing the ident from disk. Wait until the
        // drop completes to ensure that, when this method returns, the reaper will not continue
        // removing the ident in the background. This is especially important for replicated idents,
        // which may need to be reused after a rollback to stable.
        return identInfo->identState != IdentInfo::State::kBeingDropped;
    });
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

void KVDropPendingIdentReaper::_stopTrackingIdentAndNotify(
    WithLock, const Timestamp& dropTimestamp, const std::shared_ptr<IdentInfo>& identInfo) {
    auto begin = _dropPendingIdents.lower_bound(dropTimestamp);
    for (auto it = begin; it != _dropPendingIdents.end() && it->first == dropTimestamp; ++it) {
        if (it->second == identInfo) {
            invariant(_identToTimestamp.erase(identInfo->identName) == 1);
            _dropPendingIdents.erase(it);
            _identStateResetOrRemovedCV.notify_all();
            return;
        }
    }
}

}  // namespace mongo
