/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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


#include "mongo/db/storage/historical_ident_tracker.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {

namespace {

const auto getHistoricalIdentTracker = ServiceContext::declareDecoration<HistoricalIdentTracker>();

}  // namespace

HistoricalIdentTracker& HistoricalIdentTracker::get(ServiceContext* svcCtx) {
    return getHistoricalIdentTracker(svcCtx);
}

HistoricalIdentTracker& HistoricalIdentTracker::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

boost::optional<std::pair<NamespaceString, UUID>> HistoricalIdentTracker::lookup(
    const std::string& ident, Timestamp timestamp) const {
    stdx::lock_guard<Latch> lk(_mutex);

    auto mapIt = _historicalIdents.find(ident);
    if (mapIt == _historicalIdents.end()) {
        // No historical entries for this ident.
        return boost::none;
    }

    for (auto listIt = mapIt->second.begin(); listIt != mapIt->second.end(); listIt++) {
        if (timestamp >= listIt->start && timestamp <= listIt->end) {
            // Found the historical entry for the requested timestamp.
            return std::make_pair(listIt->nss, listIt->uuid);
        }
    }

    // No historical entry for the requested timestamp was found.
    return boost::none;
}

void HistoricalIdentTracker::pinAtTimestamp(Timestamp timestamp) {
    stdx::lock_guard<Latch> lk(_mutex);
    _pinnedTimestamp = timestamp;
}

void HistoricalIdentTracker::unpin() {
    stdx::lock_guard<Latch> lk(_mutex);
    _pinnedTimestamp = Timestamp::min();
}

void HistoricalIdentTracker::removeEntriesOlderThan(Timestamp timestamp) {
    Timestamp removeOlderThan =
        _pinnedTimestamp.isNull() ? timestamp : std::min(timestamp, _pinnedTimestamp);

    LOGV2_DEBUG(
        6321801, 2, "Removing historical entries older than", "timestamp"_attr = removeOlderThan);

    std::vector<std::string> keysToRemove;
    stdx::lock_guard<Latch> lk(_mutex);
    for (auto mapIt = _historicalIdents.begin(); mapIt != _historicalIdents.end(); mapIt++) {

        auto listIt = mapIt->second.begin();
        while (listIt != mapIt->second.end()) {
            if (listIt->end < removeOlderThan) {
                // This historical entry needs to be a removed, but we'll do a ranged delete later.
                LOGV2_DEBUG(6321802,
                            2,
                            "Removing historical entry",
                            "ident"_attr = mapIt->first,
                            "nss"_attr = listIt->nss,
                            "uuid"_attr = listIt->uuid,
                            "start"_attr = listIt->start,
                            "end"_attr = listIt->end);
                listIt++;
                continue;
            }

            // We need to keep this and any following historical entries. We can do a ranged delete
            // now for what we don't need.
            mapIt->second.erase(mapIt->second.begin(), listIt);
            break;
        }

        if (listIt == mapIt->second.end()) {
            // All of the historical entries need to be deleted for this ident. We'll erase the map
            // entry outside of the loop to avoid iterator invalidation.
            keysToRemove.push_back(mapIt->first);
        }
    }

    for (const auto& keyToRemove : keysToRemove) {
        _historicalIdents.erase(keyToRemove);
    }
}

void HistoricalIdentTracker::rollbackTo(Timestamp timestamp) {
    Timestamp rollbackTo =
        _pinnedTimestamp.isNull() ? timestamp : std::max(timestamp, _pinnedTimestamp);

    LOGV2_DEBUG(6321803, 2, "Rolling back historical entries to", "timestamp"_attr = rollbackTo);

    std::vector<std::string> keysToRemove;
    stdx::lock_guard<Latch> lk(_mutex);
    for (auto mapIt = _historicalIdents.begin(); mapIt != _historicalIdents.end(); mapIt++) {

        auto listIt = mapIt->second.begin();
        while (listIt != mapIt->second.end()) {
            if (listIt->end < rollbackTo) {
                // This historical entry needs to be kept.
                listIt++;
                continue;
            }

            LOGV2_DEBUG(6321804,
                        2,
                        "Removing historical entries at and beyond",
                        "ident"_attr = mapIt->first,
                        "nss"_attr = listIt->nss,
                        "uuid"_attr = listIt->uuid,
                        "start"_attr = listIt->start,
                        "end"_attr = listIt->end);

            // We need to remove this and any following historical entries. We can do a ranged
            // delete now for what we don't need.
            mapIt->second.erase(listIt, mapIt->second.end());

            if (mapIt->second.empty()) {
                // Everything was erased. The map entry will be erased outside of the loop to avoid
                // iterator invalidation.
                keysToRemove.push_back(mapIt->first);
            }

            break;
        }
    }

    for (const auto& keyToRemove : keysToRemove) {
        _historicalIdents.erase(keyToRemove);
    }
}

void HistoricalIdentTracker::_addHistoricalIdent(const std::string& ident,
                                                 const NamespaceString& nss,
                                                 const UUID& uuid,
                                                 Timestamp timestamp) {
    if (timestamp.isNull()) {
        // Standalone nodes don't use timestamps.
        return;
    }

    HistoricalIdentEntry entry{nss, uuid, /*start=*/Timestamp::min(), /*end=*/timestamp - 1};

    stdx::lock_guard<Latch> lk(_mutex);
    auto it = _historicalIdents.find(ident);
    if (it == _historicalIdents.end()) {
        // There are no historical entries for this ident yet.
        LOGV2_DEBUG(6321805,
                    2,
                    "Adding new historical entry",
                    "ident"_attr = ident,
                    "nss"_attr = entry.nss,
                    "uuid"_attr = entry.uuid,
                    "start"_attr = entry.start,
                    "end"_attr = entry.end);
        _historicalIdents.insert({ident, {std::move(entry)}});
        return;
    }

    invariant(!it->second.empty());

    // Update the start timestamp to be the last entry's end timestamp + 1.
    entry.start = it->second.back().end + 1;

    LOGV2_DEBUG(6321806,
                2,
                "Adding new historical entry",
                "ident"_attr = ident,
                "nss"_attr = entry.nss,
                "uuid"_attr = entry.uuid,
                "start"_attr = entry.start,
                "end"_attr = entry.end);
    it->second.push_back(std::move(entry));
}

}  // namespace mongo
