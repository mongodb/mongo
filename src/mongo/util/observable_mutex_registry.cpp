/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/util/observable_mutex_registry.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands/server_status/server_status.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/static_immortal.h"

namespace mongo {
MutexStats operator+(const MutexStats& lhs, const ObservableMutexRegistry::StatsRecord& rhs) {
    return {lhs + rhs.data};
}

namespace {
class ObservableMutexServerStatusSection : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    bool includeByDefault() const override {
        return false;
    }

    BSONObj generateSection(OperationContext* opCtx, const BSONElement& config) const override {
        bool listAll = false;
        if (config.isABSONObj()) {
            auto obj = config.Obj();
            listAll = obj.hasField("listAll") ? obj.getIntField("listAll") : false;
        }

        return ObservableMutexRegistry::get().report(listAll);
    }
};

auto& observableMutexSection =
    *ServerStatusSectionBuilder<ObservableMutexServerStatusSection>("lockContentionMetrics")
         .forShard()
         .forRouter();

void serialize(BSONObjBuilder& bob, const MutexAcquisitionStats& stats) {
    bob.append(ObservableMutexRegistry::kTotalAcquisitionsFieldName,
               static_cast<long long>(stats.total));
    bob.append(ObservableMutexRegistry::kTotalContentionsFieldName,
               static_cast<long long>(stats.contentions));
    bob.append(ObservableMutexRegistry::kTotalWaitCyclesFieldName,
               static_cast<long long>(stats.waitCycles));
}

void serialize(BSONObjBuilder& bob, const MutexStats& stats) {
    {
        BSONObjBuilder sub(bob.subobjStart(ObservableMutexRegistry::kExclusiveFieldName));
        serialize(sub, stats.exclusiveAcquisitions);
    }
    {
        BSONObjBuilder sub(bob.subobjStart(ObservableMutexRegistry::kSharedFieldName));
        serialize(sub, stats.sharedAcquisitions);
    }
}

void serialize(BSONArrayBuilder& builder, const ObservableMutexRegistry::StatsRecord& entry) {
    BSONObjBuilder subObjBuilder;
    invariant(entry.mutexId && entry.registered);
    subObjBuilder.append(ObservableMutexRegistry::kIdFieldName, entry.mutexId.get());
    subObjBuilder.appendDate(ObservableMutexRegistry::kRegisteredFieldName, entry.registered.get());
    serialize(subObjBuilder, entry.data);

    builder.append(subObjBuilder.obj());
}

BSONObj serializeStats(StringMap<std::vector<ObservableMutexRegistry::StatsRecord>>& statsMap,
                       bool listAll) {
    BSONObjBuilder bob;
    for (const auto& [tag, stats] : statsMap) {
        BSONObjBuilder tagSubObj(bob.subobjStart(tag));
        const auto sum = std::accumulate(stats.begin(), stats.end(), MutexStats{});
        serialize(tagSubObj, sum);

        if (MONGO_unlikely(listAll)) {
            BSONArrayBuilder mutexSubArray(
                tagSubObj.subarrayStart(ObservableMutexRegistry::kMutexFieldName));
            for (const auto& entry : stats) {
                if (!entry.mutexId || !entry.registered) {
                    // The listAll output should ignore stats from invalidated mutexes.
                    continue;
                }
                serialize(mutexSubArray, entry);
            }
        }
    }
    return bob.obj();
}
}  // namespace

ObservableMutexRegistry& ObservableMutexRegistry::get() {
    static StaticImmortal<ObservableMutexRegistry> obj;
    return *obj;
}

BSONObj ObservableMutexRegistry::report(bool listAll) {
    auto stats = _collectStats();
    return serializeStats(stats, listAll);
}

StringMap<std::vector<ObservableMutexRegistry::StatsRecord>>
ObservableMutexRegistry::_collectStats() {
    stdx::lock_guard lk(_collectionMutex);

    std::list<NewMutexEntry> newEntries;
    {
        stdx::lock_guard lk(_registrationMutex);
        newEntries.splice(newEntries.end(), _newMutexEntries);
    }
    // Integrate the new entries into `_mutexEntries`.
    for (NewMutexEntry& entry : newEntries) {
        _mutexEntries[std::move(entry.tag)].push_back({.id = _nextMutexId++,
                                                       .registrationTime = entry.registrationTime,
                                                       .token = std::move(entry.token)});
    }

    StringMap<std::vector<StatsRecord>> statsMap;
    for (auto& [tag, entries] : _mutexEntries) {
        auto& records = statsMap[tag];
        MutexStats* removedTotal = nullptr;
        for (auto it = entries.begin(); it != entries.end();) {
            const auto stats = it->token->getStats();
            if (MONGO_unlikely(!it->token->isValid())) {
                if (!removedTotal) {
                    removedTotal = &_removedTokensSnapshots[tag];
                }
                *removedTotal += stats;
                it = entries.erase(it);
            } else {
                records.push_back(StatsRecord{stats, it->id, it->registrationTime});
                ++it;
            }
        }
    }

    _includeRemovedSnapshots(lk, statsMap);
    return statsMap;
}

void ObservableMutexRegistry::_includeRemovedSnapshots(
    WithLock, StringMap<std::vector<StatsRecord>>& statsMap) {
    for (const auto& [tag, stats] : _removedTokensSnapshots) {
        // The mutexId and registered field within entry are left blank to indicate that these stats
        // came from invalidated mutexes.
        StatsRecord entry{stats};
        statsMap[tag].push_back(entry);
    }
}
}  // namespace mongo
