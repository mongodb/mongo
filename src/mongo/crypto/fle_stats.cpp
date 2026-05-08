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

#include "mongo/crypto/fle_stats.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/crypto/encryption_fields_util.h"
#include "mongo/crypto/fle_options_gen.h"
#include "mongo/util/system_tick_source.h"
#include "mongo/util/testing_options_gen.h"

namespace mongo {

namespace {
// We only track fle stats on the shard.
auto& fleStatusSection =
    *ServerStatusSectionBuilder<FLEStatusSection>("fle").forShard().bind(globalSystemTickSource());
}  // namespace

RateLimitedCounter::RateLimitedCounter(Milliseconds cooldown, TickSource* tickSource)
    : _tickSource(tickSource),
      _ticksPerPeriod(static_cast<TickSource::Tick>(
          cooldown.count() * static_cast<double>(_tickSource->getTicksPerSecond()) /
          Milliseconds::period::den)) {
    // set _lastUpdate such that the next increment call always increments
    _lastUpdate.store(_tickSource->getTicks() - _ticksPerPeriod);
}

bool RateLimitedCounter::increment() {
    TickSource::Tick last = _lastUpdate.load();
    TickSource::Tick now = _tickSource->getTicks();

    if ((now - last) < _ticksPerPeriod) {
        return false;  // too early
    }

    if (_lastUpdate.compareAndSwap(&last, now)) {
        _count.fetchAndAddRelaxed(1);
        return true;
    }
    return false;  // lost race with another thread
}

FLEPerCollectionStats::FLEPerCollectionStats(Milliseconds cooldown, TickSource* tickSource)
    : findOpCounter(cooldown, tickSource),
      updateOpCounter(cooldown, tickSource),
      deleteOpCounter(cooldown, tickSource) {};

FLEPerCollectionStats& FLEPerCollectionStats::operator+=(const FLEPerCollectionStats& rhs) {
    insertOpCounter.fetchAndAddRelaxed(rhs.insertOpCounter.loadRelaxed());
    findOpCounter.forceIncrement(rhs.findOpCounter.getCount());
    deleteOpCounter.forceIncrement(rhs.deleteOpCounter.getCount());
    updateOpCounter.forceIncrement(rhs.updateOpCounter.getCount());
    return *this;
}

void FLEPerCollectionStats::serialize(BSONObjBuilder* builder) const {
    *builder << "inserts" << insertOpCounter.loadRelaxed();
    *builder << "finds" << findOpCounter.getCount();
    *builder << "updates" << updateOpCounter.getCount();
    *builder << "deletes" << deleteOpCounter.getCount();
}


FLEStatusSection::FLEStatusSection(std::string name, ClusterRole role, TickSource* tickSource)
    : ServerStatusSection(std::move(name), std::move(role)),
      _tickSource(tickSource),
      _deletedCollStats(Milliseconds{0}, tickSource) {}

FLEStatusSection& FLEStatusSection::get() {
    return fleStatusSection;
}

BSONObj FLEStatusSection::generateSection(OperationContext* opCtx,
                                          const BSONElement& configElement) const {
    BSONObjBuilder builder;

    if (gTestingDiagnosticsEnabledAtStartup &&
        gUnsupportedDangerousTestingFLEDiagnosticsEnabledAtStartup) {
        auto sub = BSONObjBuilder(builder.subobjStart("emuBinaryStats"));
        sub << "calls" << emuBinaryCalls.loadRelaxed();
        sub << "suboperations" << emuBinarySuboperation.loadRelaxed();
        sub << "totalMillis" << emuBinaryTotalMillis.loadRelaxed();
    }

    {
        FLEIndexTypeStats temp;
        {
            std::lock_guard<std::mutex> lock(_indexTypeMutex);
            temp = _indexTypeStats;
        }
        auto sub = BSONObjBuilder(builder.subobjStart("indexTypeStats"));
        temp.serialize(&sub);
    }

    {
        FLEPerCollectionStats aggregate(Milliseconds{0}, _tickSource);
        auto statsMap = _collStatsMap.getUnderlyingSnapshot();
        for (auto const& [_, statsp] : *statsMap) {
            aggregate += *statsp;
        }
        aggregate += _deletedCollStats;
        auto sub = BSONObjBuilder(builder.subobjStart("operationCounters"));
        aggregate.serialize(&sub);
    }

    return builder.obj();
}


FLEStatusSection::EmuBinaryTracker FLEStatusSection::makeEmuBinaryTracker() {
    return EmuBinaryTracker(this, gTestingDiagnosticsEnabledAtStartup);
}

void FLEStatusSection::updateIndexTypeStats(const EncryptedFieldConfig& efc, bool subtract) {
    const std::int64_t delta = (subtract ? -1 : 1);
    FLEIndexTypeStats deltas;
    visitQueryTypeConfigs(
        efc,
        [&deltas, delta](const EncryptedField& field, const QueryTypeConfig& qtc) {
            switch (qtc.getQueryType()) {
                case QueryTypeEnum::Equality:
                    deltas.setEquality(delta);
                    break;
                case QueryTypeEnum::Range:
                    deltas.setRange(delta);
                    break;
                case QueryTypeEnum::RangePreviewDeprecated:
                    deltas.setRangePreview(delta);
                    break;
                case QueryTypeEnum::SubstringPreview:
                    deltas.setSubstringPreview(delta);
                    break;
                case QueryTypeEnum::Suffix:
                    deltas.setSuffix(delta);
                    break;
                case QueryTypeEnum::SuffixPreviewDeprecated:
                    deltas.setSuffixPreview(delta);
                    break;
                case QueryTypeEnum::Prefix:
                    deltas.setPrefix(delta);
                    break;
                case QueryTypeEnum::PrefixPreviewDeprecated:
                    deltas.setPrefixPreview(delta);
                    break;
                default:
                    MONGO_UNREACHABLE;
            };
            return false;
        },
        [&deltas, delta](const EncryptedField&) {
            deltas.setUnindexed(delta);
            return false;
        });

    std::lock_guard<std::mutex> lock(_indexTypeMutex);
    FLEStatsUtil::accumulateStats(_indexTypeStats, deltas);
}

void FLEStatusSection::updateStatsOnRegisterCollection(const NamespaceString& nss,
                                                       const EncryptedFieldConfig& efc) {
    updateIndexTypeStats(efc, false);
    registerCollectionStats(nss, efc);
}

void FLEStatusSection::updateStatsOnDeregisterCollection(const NamespaceString& nss,
                                                         const EncryptedFieldConfig& efc) {
    updateIndexTypeStats(efc, true);
    deregisterCollectionStats(nss);
}

void FLEStatusSection::clearIndexTypeStats() {
    std::lock_guard<std::mutex> lock(_indexTypeMutex);
    _indexTypeStats = {};
}

void FLEStatusSection::incrementInsertCount(const NamespaceString& nss,
                                            const EncryptedFieldConfig& efc) {
    registerCollectionStats(nss, efc)->insertOpCounter.fetchAndAddRelaxed(1);
}

void FLEStatusSection::incrementFindCount(const NamespaceString& nss,
                                          const EncryptedFieldConfig& efc) {
    registerCollectionStats(nss, efc)->findOpCounter.increment();
}

void FLEStatusSection::incrementUpdateCount(const NamespaceString& nss,
                                            const EncryptedFieldConfig& efc) {
    registerCollectionStats(nss, efc)->updateOpCounter.increment();
}

void FLEStatusSection::incrementDeleteCount(const NamespaceString& nss,
                                            const EncryptedFieldConfig& efc) {
    registerCollectionStats(nss, efc)->deleteOpCounter.increment();
}

std::shared_ptr<FLEPerCollectionStats> FLEStatusSection::registerCollectionStats(
    const NamespaceString& nss, const EncryptedFieldConfig& efc) {
    if (auto statsp = _collStatsMap.find(nss); statsp != nullptr) {
        return statsp;
    }
    return _collStatsMap.getOrEmplace(
        nss, calculateOpCounterIncrementCooldownValue(efc), _tickSource);
}

void FLEStatusSection::deregisterCollectionStats(const NamespaceString& nss) {
    auto statsp = _collStatsMap.find(nss);
    if (!statsp) {
        return;
    }
    _collStatsMap.erase(nss);
    _deletedCollStats += *statsp;
}

Milliseconds FLEStatusSection::calculateOpCounterIncrementCooldownValue(
    const EncryptedFieldConfig& efc) {
    // These coefficients are in nanoseconds and determined from empirical testing in SPM-4539
    constexpr std::int64_t kCoefficientA = 1327732;
    constexpr std::int64_t kCoefficientB = 329137;
    constexpr std::int64_t kCoefficientC = 1;
    constexpr std::int64_t kTagLimit = 325000;
    constexpr std::int64_t klog2OfMaxEscSize = 32;

    std::int64_t maxCf = 0;
    visitQueryTypeConfigs(efc, [&maxCf](const EncryptedField& field, const QueryTypeConfig& qtc) {
        maxCf = std::max(qtc.getContention(), maxCf);
        return false;
    });
    const auto w =
        (kCoefficientA * maxCf * klog2OfMaxEscSize) + (kCoefficientB * kTagLimit) + kCoefficientC;
    return duration_cast<Milliseconds>(Nanoseconds{w});
}

}  // namespace mongo
