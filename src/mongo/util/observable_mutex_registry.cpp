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

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/ctype.h"
#include "mongo/util/static_immortal.h"

#include <string_view>

#include <fmt/format.h>

namespace mongo {
MutexStats operator+(const MutexStats& lhs, const ObservableMutexRegistry::StatsRecord& rhs) {
    return {lhs + rhs.data};
}

namespace {

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
    if (entry.instanceLabel) {
        subObjBuilder.append(ObservableMutexRegistry::kInstanceLabelFieldName,
                             entry.instanceLabel.get());
    }
    subObjBuilder.appendDate(ObservableMutexRegistry::kRegisteredFieldName, entry.registered.get());
    serialize(subObjBuilder, entry.data);

    builder.append(subObjBuilder.obj());
}

BSONObj serializeStats(StringMap<std::vector<ObservableMutexRegistry::StatsRecord>>& statsMap,
                       bool listAll) {
    BSONObjBuilder bob;
    // Using a `set` to store tags and ensure the serialized stats are sorted by their tags.
    std::set<std::string_view> tags;
    for (const auto& [tag, _] : statsMap) {
        tags.insert(tag);
    }

    for (auto tag : tags) {
        auto& stats = statsMap[tag];
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


// Checks if a tag is a valid OpenTelemetry metric name segment.
bool isValidSegment(std::string_view seg) {
    if (seg.empty() || seg.front() < 'a' || seg.front() > 'z') {
        return false;
    }
    bool hasUpper = false, hasUnderscore = false;
    for (size_t i = 1; i < seg.size(); ++i) {
        const char c = seg[i];
        if (c == '_') {
            hasUnderscore = true;
            if (i + 1 == seg.size() || !ctype::isAlnum(seg[i + 1])) {
                return false;
            }
        } else if (ctype::isUpper(c)) {
            hasUpper = true;
        } else if (!ctype::isLower(c) && !ctype::isDigit(c)) {
            return false;
        }
    }
    return !(hasUpper && hasUnderscore);
}

void ObservableMutexRegistry::_validateTag(std::string_view tag) {
    uassert(ErrorCodes::InvalidOptions,
            fmt::format("mutex tag is not a valid OTel metric name segment: '{}'", tag),
            isValidSegment(tag));
}

ObservableMutexRegistry& ObservableMutexRegistry::get() {
    static StaticImmortal<ObservableMutexRegistry> obj;
    return *obj;
}

BSONObj ObservableMutexRegistry::report(bool listAll) {
    auto stats = _collectStats();
    return serializeStats(stats, listAll);
}

StringMap<MutexStats> ObservableMutexRegistry::statsPerTag() {
    auto rawStats = _collectStats();
    StringMap<MutexStats> result;
    for (auto& [tag, records] : rawStats) {
        result[tag] = std::accumulate(records.begin(), records.end(), MutexStats{});
    }
    return result;
}

StringMap<std::vector<ObservableMutexRegistry::StatsRecord>>
ObservableMutexRegistry::_collectStats() {
    std::lock_guard lk(_collectionMutex);

    std::list<NewMutexEntry> newEntries;
    {
        std::lock_guard lk(_registrationMutex);
        newEntries.splice(newEntries.end(), _newMutexEntries);
    }
    // Integrate the new entries into `_mutexEntries`.
    for (NewMutexEntry& entry : newEntries) {
        _mutexEntries[std::move(entry.tag)].push_back(
            {.id = _nextMutexId++,
             .instanceLabel = std::move(entry.instanceLabel),
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
                records.push_back(
                    StatsRecord{stats, it->id, it->registrationTime, it->instanceLabel});
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
