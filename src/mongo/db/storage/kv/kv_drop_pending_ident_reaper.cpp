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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/storage/kv/kv_drop_pending_ident_reaper.h"

#include <algorithm>

#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/logv2/log.h"

namespace mongo {

KVDropPendingIdentReaper::KVDropPendingIdentReaper(KVEngine* engine) : _engine(engine) {}

void KVDropPendingIdentReaper::addDropPendingIdent(const Timestamp& dropTimestamp,
                                                   const NamespaceString& nss,
                                                   StringData ident) {
    stdx::lock_guard<Latch> lock(_mutex);
    const auto equalRange = _dropPendingIdents.equal_range(dropTimestamp);
    const auto& lowerBound = equalRange.first;
    const auto& upperBound = equalRange.second;
    auto matcher = [ident](const auto& pair) { return pair.second.ident == ident; };
    if (std::find_if(lowerBound, upperBound, matcher) == upperBound) {
        IdentInfo info;
        info.nss = nss;
        info.ident = ident.toString();
        _dropPendingIdents.insert(std::make_pair(dropTimestamp, info));
    } else {
        LOGV2_FATAL_NOTRACE(51023,
                            "Failed to add drop-pending ident {ident} ({nss}) with drop timestamp "
                            "{dropTimestamp}: duplicate timestamp and ident pair.",
                            "ident"_attr = ident,
                            "nss"_attr = nss,
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

std::set<std::string> KVDropPendingIdentReaper::getAllIdents() const {
    stdx::lock_guard<Latch> lock(_mutex);
    std::set<std::string> idents;
    for (const auto& entry : _dropPendingIdents) {
        const auto& identInfo = entry.second;
        const auto& ident = identInfo.ident;
        idents.insert(ident);
    }
    return idents;
}

void KVDropPendingIdentReaper::dropIdentsOlderThan(OperationContext* opCtx, const Timestamp& ts) {
    DropPendingIdents toDrop;
    {
        stdx::lock_guard<Latch> lock(_mutex);
        for (auto it = _dropPendingIdents.cbegin();
             it != _dropPendingIdents.cend() && it->first < ts;
             ++it) {
            toDrop.insert(*it);
        }
    }

    if (toDrop.empty()) {
        return;
    }


    {
        // Guards against catalog changes while dropping idents using KVEngine::dropIdent().
        Lock::GlobalLock globalLock(opCtx, MODE_IX);

        for (const auto& timestampAndIdentInfo : toDrop) {
            const auto& dropTimestamp = timestampAndIdentInfo.first;
            const auto& identInfo = timestampAndIdentInfo.second;
            const auto& nss = identInfo.nss;
            const auto& ident = identInfo.ident;
            LOGV2(
                22237,
                "Completing drop for ident {ident} (ns: {nss}) with drop timestamp {dropTimestamp}",
                "ident"_attr = ident,
                "nss"_attr = nss,
                "dropTimestamp"_attr = dropTimestamp);
            WriteUnitOfWork wuow(opCtx);
            auto status = _engine->dropIdent(opCtx, opCtx->recoveryUnit(), ident);
            if (!status.isOK()) {
                LOGV2_FATAL_NOTRACE(
                    51022,
                    "Failed to remove drop-pending ident {ident}(ns: {nss}) with drop "
                    "timestamp {dropTimestamp}: {status}",
                    "ident"_attr = ident,
                    "nss"_attr = nss,
                    "dropTimestamp"_attr = dropTimestamp,
                    "status"_attr = status);
            }
            wuow.commit();
        }
    }

    {
        // Entries must be removed AFTER drops are completed, so that getEarliestDropTimestamp()
        // returns appropriate results.
        stdx::lock_guard<Latch> lock(_mutex);
        for (const auto& timestampAndIdentInfo : toDrop) {
            const auto& dropTimestamp = timestampAndIdentInfo.first;
            // This may return zero if _dropPendingIdents was cleared using clearDropPendingState().
            _dropPendingIdents.erase(dropTimestamp);
        }
    }
}

void KVDropPendingIdentReaper::clearDropPendingState() {
    stdx::lock_guard<Latch> lock(_mutex);
    _dropPendingIdents.clear();
}

}  // namespace mongo
