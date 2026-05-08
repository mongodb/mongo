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


#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/crypto/fle_stats_gen.h"
#include "mongo/db/commands/server_status/server_status.h"
#include "mongo/db/operation_context.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/concurrent_shared_values_map.h"
#include "mongo/util/modules.h"
#include "mongo/util/tick_source.h"
#include "mongo/util/timer.h"

#include <mutex>

namespace MONGO_MOD_PUBLIC mongo {

namespace FLEStatsUtil {
static void accumulateStats(ECStats& left, const ECStats& right) {
    left.setRead(left.getRead() + right.getRead());
    left.setInserted(left.getInserted() + right.getInserted());
    left.setUpdated(left.getUpdated() + right.getUpdated());
    left.setDeleted(left.getDeleted() + right.getDeleted());
}
static void accumulateStats(ECOCStats& left, const ECOCStats& right) {
    left.setRead(left.getRead() + right.getRead());
    left.setDeleted(left.getDeleted() + right.getDeleted());
}
static void accumulateStats(FLEIndexTypeStats& left, const FLEIndexTypeStats& right) {
    left.setEquality(left.getEquality() + right.getEquality());
    left.setRange(left.getRange() + right.getRange());
    left.setSuffix(left.getSuffix() + right.getSuffix());
    left.setPrefix(left.getPrefix() + right.getPrefix());
    left.setRangePreview(left.getRangePreview() + right.getRangePreview());
    left.setSubstringPreview(left.getSubstringPreview() + right.getSubstringPreview());
    left.setSuffixPreview(left.getSuffixPreview() + right.getSuffixPreview());
    left.setPrefixPreview(left.getPrefixPreview() + right.getPrefixPreview());
    left.setUnindexed(left.getUnindexed() + right.getUnindexed());
}
}  // namespace FLEStatsUtil

/**
 * Thread-safe counter that only increments if a cooldown period has passed since the last
 * increment.
 */
class RateLimitedCounter {
public:
    RateLimitedCounter(Milliseconds cooldown, TickSource* tickSource);

    // Try to increment the counter by 1.
    // Returns true on successful increment, or false if too early.
    bool increment();

    // Returns the current counter value.
    long long getCount() const {
        return _count.loadRelaxed();
    }

    // Increment the counter by the given amount regardless of the cooldown period.
    void forceIncrement(long long amount = 1) {
        _count.fetchAndAddRelaxed(amount);
    }

private:
    TickSource* _tickSource;
    const TickSource::Tick _ticksPerPeriod;
    AtomicWord<long long> _count{0};
    Atomic<TickSource::Tick> _lastUpdate{0};
};

/**
 * Tracks and reports operation statistics about individual FLE2 collections.
 */
class FLEPerCollectionStats {
public:
    FLEPerCollectionStats(Milliseconds cooldown, TickSource* tickSource);

    FLEPerCollectionStats& operator+=(const FLEPerCollectionStats& rhs);

    void serialize(BSONObjBuilder* builder) const;

    AtomicWord<long long> insertOpCounter = 0;
    RateLimitedCounter findOpCounter;
    RateLimitedCounter updateOpCounter;
    RateLimitedCounter deleteOpCounter;
};

/**
 * Tracks and reports statistics about the server-side Queryable Encryption integration.
 */
class FLEStatusSection : public ServerStatusSection {
public:
    // Creates a section manager which tracks operation time with provided clock source.
    FLEStatusSection(std::string name, ClusterRole role, TickSource* tickSource);

    // Return the global status section Singleton
    static FLEStatusSection& get();

    static Milliseconds calculateOpCounterIncrementCooldownValue(const EncryptedFieldConfig& efc);

    // Report FLE metrics at all times
    bool includeByDefault() const final {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx, const BSONElement& configElement) const final;

    /**
     *  An object by which a caller may report their interactions with Emulated Binary Search.
     *
     *  Every time a caller performs emuBinary, this object will report the attempt. After the
     *  caller is done, this object will report the total elapsed wallclock time.
     */
    class EmuBinaryTracker {
        friend FLEStatusSection;

    public:
        ~EmuBinaryTracker() {
            if (_active) {
                _section->emuBinaryTotalMillis.fetchAndAddRelaxed(_timer.millis());
            }
        }

        void recordSuboperation() {
            if (_active) {
                _section->emuBinarySuboperation.fetchAndAddRelaxed(1);
            }
        }

    private:
        explicit EmuBinaryTracker(FLEStatusSection* section, bool active)
            : _section(section), _active(active), _timer(_section->_tickSource) {
            if (_active) {
                _section->emuBinaryCalls.fetchAndAddRelaxed(1);
            }
        }

        FLEStatusSection* _section;
        bool _active;
        Timer _timer;
    };

    EmuBinaryTracker makeEmuBinaryTracker();

    void updateStatsOnRegisterCollection(const NamespaceString& nss,
                                         const EncryptedFieldConfig& efc);
    void updateStatsOnDeregisterCollection(const NamespaceString& nss,
                                           const EncryptedFieldConfig& efc);
    void clearIndexTypeStats();

    void incrementInsertCount(const NamespaceString& nss, const EncryptedFieldConfig& efc);
    void incrementFindCount(const NamespaceString& nss, const EncryptedFieldConfig& efc);
    void incrementUpdateCount(const NamespaceString& nss, const EncryptedFieldConfig& efc);
    void incrementDeleteCount(const NamespaceString& nss, const EncryptedFieldConfig& efc);

private:
    void updateIndexTypeStats(const EncryptedFieldConfig& efc, bool subtract);

    std::shared_ptr<FLEPerCollectionStats> registerCollectionStats(const NamespaceString& nss,
                                                                   const EncryptedFieldConfig& efc);
    void deregisterCollectionStats(const NamespaceString& nss);

    TickSource* _tickSource;

    AtomicWord<long long> emuBinaryCalls;
    AtomicWord<long long> emuBinarySuboperation;
    AtomicWord<long long> emuBinaryTotalMillis;

    // Tracks and reports statistics about how many collections in the catalog use each of the
    // Queryable Encryption index types, and how many collections use unindexed encryption.
    mutable std::mutex _indexTypeMutex;
    FLEIndexTypeStats _indexTypeStats;

    // Holds operation counters for each active FLE2 collection.
    ConcurrentSharedValuesMap<NamespaceString, FLEPerCollectionStats> _collStatsMap;

    // Holds operation counters for deleted FLE2 collections.
    FLEPerCollectionStats _deletedCollStats;
};

}  // namespace MONGO_MOD_PUBLIC mongo
