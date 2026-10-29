// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/counter.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/modules.h"
#include "mongo/util/timer.h"

#include <list>
#include <mutex>

namespace mongo {

class [[MONGO_MOD_PARENT_PRIVATE]] CheckMetadataConsistencyStatistics {
public:
    struct StatisticsRecorder {
        ~StatisticsRecorder() {
            std::lock_guard lk(stats->_listMutex);
            owningCumulativeCounterMillis->increment(durationCount<Milliseconds>(pos->elapsed()));
            owningList->erase(pos);
        }

        StatisticsRecorder(const StatisticsRecorder&) = delete;
        StatisticsRecorder(StatisticsRecorder&&) = delete;

    private:
        StatisticsRecorder(CheckMetadataConsistencyStatistics* stats,
                           std::list<Timer>* owningList,
                           Counter64* owningCumulativeCounterMillis,
                           std::list<Timer>::iterator pos)
            : stats(stats),
              owningList(owningList),
              owningCumulativeCounterMillis(owningCumulativeCounterMillis),
              pos(pos) {};

        CheckMetadataConsistencyStatistics* stats;
        std::list<Timer>* owningList;
        Counter64* owningCumulativeCounterMillis;
        std::list<Timer>::iterator pos;

        friend class CheckMetadataConsistencyStatistics;
    };

    void report(BSONObjBuilder& builder) const {
        builder.append("numberOfInconsistenciesFound", _inconsistenciesFound.get());

        builder.append("numberOfDatabasesChecked", _numDatabasesChecked.get());
        builder.append("numberOfCollectionsChecked", _numCollectionsChecked.get());
        builder.append("numberOfChunksChecked", _numChunksChecked.get());

        int64_t collLockHeld;
        Microseconds collLockHeldTime{0};
        int64_t dbLockHeld;
        Microseconds dbLockHeldTime{0};

        {
            std::lock_guard lk(_listMutex);

            for (const auto& timer : _collLockTimers) {
                collLockHeldTime += timer.elapsed();
            }
            collLockHeld = _collLockTimers.size();

            for (const auto& timer : _dbLockTimers) {
                dbLockHeldTime += timer.elapsed();
            }
            dbLockHeld = _dbLockTimers.size();
        }

        builder.append("activeDdlLocksHeldForDatabase", dbLockHeld);
        builder.append("activeDdlLocksHeldForDatabaseDurationMillis",
                       durationCount<Milliseconds>(dbLockHeldTime));
        builder.append("activeDdlLocksHeldForCollection", collLockHeld);
        builder.append("activeDdlLocksHeldForCollectionDurationMillis",
                       durationCount<Milliseconds>(collLockHeldTime));

        builder.append("ddlLockHeldForDatabaseDurationMillis",
                       _dbLockHeldCumulativeDurationMillis.get());
        builder.append("ddlLockHeldForCollectionDurationMillis",
                       _collLockHeldCumulativeDurationMillis.get());
    }

    StatisticsRecorder registerDatabaseDDLLockForStatistics() {
        std::lock_guard lk(_listMutex);
        _dbLockTimers.emplace_back();
        return StatisticsRecorder{this,
                                  &_dbLockTimers,
                                  &_dbLockHeldCumulativeDurationMillis,
                                  std::prev(_dbLockTimers.end())};
    }

    StatisticsRecorder registerCollectionDDLLockForStatistics() {
        std::lock_guard lk(_listMutex);
        _collLockTimers.emplace_back();
        return StatisticsRecorder{this,
                                  &_collLockTimers,
                                  &_collLockHeldCumulativeDurationMillis,
                                  std::prev(_collLockTimers.end())};
    }

    void registerCollectionChecked() {
        _numCollectionsChecked.incrementRelaxed();
    }

    void registerChunksChecked(uint64_t numChunks = 1) {
        _numChunksChecked.incrementRelaxed(numChunks);
    }

    void registerDatabaseChecked() {
        _numDatabasesChecked.incrementRelaxed();
    }

    void incrementInconsistenciesFound(uint64_t numInconsistencies = 1) {
        _inconsistenciesFound.incrementRelaxed(numInconsistencies);
    }

private:
    // Counters for checkMetadataConsistency command execution.
    Counter64 _numChunksChecked;
    Counter64 _numCollectionsChecked;
    Counter64 _numDatabasesChecked;

    mutable std::mutex _listMutex;
    std::list<Timer> _dbLockTimers;
    std::list<Timer> _collLockTimers;

    Counter64 _dbLockHeldCumulativeDurationMillis;
    Counter64 _collLockHeldCumulativeDurationMillis;

    Counter64 _inconsistenciesFound;
};
}  // namespace mongo
