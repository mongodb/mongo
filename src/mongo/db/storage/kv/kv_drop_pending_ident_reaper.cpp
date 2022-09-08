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


#include "mongo/platform/basic.h"

#include "mongo/db/storage/kv/kv_drop_pending_ident_reaper.h"

#include <algorithm>

#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/storage/ident.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {

KVDropPendingIdentReaper::KVDropPendingIdentReaper(KVEngine* engine) : _engine(engine) {}

void KVDropPendingIdentReaper::addDropPendingIdent(const Timestamp& dropTimestamp,
                                                   std::shared_ptr<Ident> ident,
                                                   StorageEngine::DropIdentCallback&& onDrop) {
    stdx::lock_guard<Latch> lock(_mutex);
    const auto equalRange = _dropPendingIdents.equal_range(dropTimestamp);
    const auto& lowerBound = equalRange.first;
    const auto& upperBound = equalRange.second;
    auto matcher = [ident](const auto& pair) { return pair.second.identName == ident->getIdent(); };
    if (std::find_if(lowerBound, upperBound, matcher) == upperBound) {
        IdentInfo info;
        info.identName = ident->getIdent();
        info.isDropped = false;
        info.dropToken = ident;
        info.onDrop = std::move(onDrop);
        _dropPendingIdents.insert(std::make_pair(dropTimestamp, info));
    } else {
        LOGV2_FATAL_NOTRACE(51023,
                            "Failed to add drop-pending ident, duplicate timestamp and ident pair",
                            "ident"_attr = ident->getIdent(),
                            "dropTimestamp"_attr = dropTimestamp);
    }
}

boost::optional<Timestamp> KVDropPendingIdentReaper::getEarliestDropTimestamp() const {
    stdx::lock_guard<Latch> lock(_mutex);
    auto it = _dropPendingIdents.cbegin();
    if (it == _dropPendingIdents.cend()) {
        return boost::none;
    }
    return it->first;
}

std::set<std::string> KVDropPendingIdentReaper::getAllIdentNames() const {
    stdx::lock_guard<Latch> lock(_mutex);
    std::set<std::string> identNames;
    for (const auto& entry : _dropPendingIdents) {
        const auto& identInfo = entry.second;
        const auto& identName = identInfo.identName;
        identNames.insert(identName);
    }
    return identNames;
}

void KVDropPendingIdentReaper::dropIdentsOlderThan(OperationContext* opCtx, const Timestamp& ts) {
    DropPendingIdents toDrop;
    {
        stdx::lock_guard<Latch> lock(_mutex);
        for (auto it = _dropPendingIdents.cbegin();
             it != _dropPendingIdents.cend() && (it->first < ts || it->first == Timestamp::min());
             ++it) {
            // This collection/index satisfies the 'ts' requirement to be safe to drop, but we must
            // also check that there are no active operations remaining that still retain a
            // reference by which to access the collection/index data.
            if (it->second.dropToken.expired()) {
                toDrop.insert(*it);
            }
        }
    }

    if (toDrop.empty()) {
        return;
    }

    for (auto& timestampAndIdentInfo : toDrop) {
        // Guards against catalog changes while dropping idents using KVEngine::dropIdent(). Yields
        // after dropping each ident.
        writeConflictRetry(opCtx, "dropIdentsOlderThan", "", [&] {
            Lock::GlobalLock globalLock(opCtx, MODE_IX);

            const auto& dropTimestamp = timestampAndIdentInfo.first;
            auto& identInfo = timestampAndIdentInfo.second;
            const auto& identName = identInfo.identName;
            LOGV2(22237,
                  "Completing drop for ident",
                  "ident"_attr = identName,
                  "dropTimestamp"_attr = dropTimestamp);
            WriteUnitOfWork wuow(opCtx);
            auto status =
                _engine->dropIdent(opCtx->recoveryUnit(), identName, std::move(identInfo.onDrop));
            if (!status.isOK()) {
                if (status == ErrorCodes::ObjectIsBusy) {
                    LOGV2(6936300,
                          "Drop-pending ident is still in use",
                          "ident"_attr = identName,
                          "dropTimestamp"_attr = dropTimestamp,
                          "error"_attr = status);
                    return;
                }
                LOGV2_FATAL_NOTRACE(51022,
                                    "Failed to remove drop-pending ident",
                                    "ident"_attr = identName,
                                    "dropTimestamp"_attr = dropTimestamp,
                                    "error"_attr = status);
            }

            // Ident drops are non-transactional and cannot be rolled back. So this does not need to
            // be in an onCommit handler.
            identInfo.isDropped = true;

            wuow.commit();
            LOGV2(6776600,
                  "The ident was successfully dropped",
                  "ident"_attr = identName,
                  "dropTimestamp"_attr = dropTimestamp);
        });
    }

    {
        // Entries must be removed AFTER drops are completed, so that getEarliestDropTimestamp()
        // returns correct results while the success of the drop operations above are uncertain.

        stdx::lock_guard<Latch> lock(_mutex);
        for (const auto& timestampAndIdentInfo : toDrop) {
            if (!timestampAndIdentInfo.second.isDropped) {
                // This ident was not dropped. Skip removing it from the drop pending list.
                continue;
            }

            // Some idents with drop timestamps safe to drop may not have been dropped because they
            // are still in use by another operation. Therefore, we must iterate the entries in the
            // multimap matching a particular timestamp and erase only the entry with a match on the
            // ident as well as the timestamp.
            auto beginEndPair = _dropPendingIdents.equal_range(timestampAndIdentInfo.first);
            for (auto it = beginEndPair.first; it != beginEndPair.second;) {
                if (it->second.identName == timestampAndIdentInfo.second.identName) {
                    it = _dropPendingIdents.erase(it);
                    break;
                } else {
                    ++it;
                }
            }
        }
    }
}

void KVDropPendingIdentReaper::clearDropPendingState() {
    stdx::lock_guard<Latch> lock(_mutex);
    _dropPendingIdents.clear();
}

}  // namespace mongo
