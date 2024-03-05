/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/stats/read_preference_metrics.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/read_preference_metrics_gen.h"
#include "mongo/util/decorable.h"

namespace mongo {
namespace {
const auto ReadPreferenceMetricsDecoration =
    ServiceContext::declareDecoration<ReadPreferenceMetrics>();
}  // namespace

ReadPreferenceMetrics* ReadPreferenceMetrics::get(ServiceContext* service) {
    return &ReadPreferenceMetricsDecoration(service);
}

ReadPreferenceMetrics* ReadPreferenceMetrics::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

void ReadPreferenceMetrics::recordReadPreference(ReadPreferenceSetting readPref,
                                                 bool isInternal,
                                                 bool isPrimary) {
    auto& counters = isPrimary ? primaryCounters : secondaryCounters;
    counters.increment(readPref, isInternal);
}

void ReadPreferenceMetrics::Counters::increment(ReadPreferenceSetting readPref, bool isInternal) {
    switch (readPref.pref) {
        case ReadPreference::PrimaryOnly:
            primary.increment(isInternal);
            break;
        case ReadPreference::PrimaryPreferred:
            primaryPreferred.increment(isInternal);
            break;
        case ReadPreference::SecondaryOnly:
            secondary.increment(isInternal);
            break;
        case ReadPreference::SecondaryPreferred:
            secondaryPreferred.increment(isInternal);
            break;
        case ReadPreference::Nearest:
            nearest.increment(isInternal);
            break;
        default:
            MONGO_UNREACHABLE;
    }

    // For primary read preference, the tag set will be empty, which is represented by an empty BSON
    // array. For other read preference values without tags, the tag set contains one empty document
    // in the BSON array. See the TagSet class definition for more details.
    if (readPref.tags != TagSet::primaryOnly() && readPref.tags != TagSet()) {
        tagged.increment(isInternal);
    }
}

void ReadPreferenceMetrics::Counter::increment(bool isInternal) {
    auto& counter = isInternal ? internal : external;
    counter.fetchAndAdd(1);
}

ReadPrefOps ReadPreferenceMetrics::Counter::toReadPrefOps() const {
    ReadPrefOps ops;
    ops.setInternal(internal.load());
    ops.setExternal(external.load());
    return ops;
}

void ReadPreferenceMetrics::Counters::flushCounters(ReadPrefDoc* doc) {
    doc->setPrimary(primary.toReadPrefOps());
    doc->setPrimaryPreferred(primaryPreferred.toReadPrefOps());
    doc->setSecondary(secondary.toReadPrefOps());
    doc->setSecondaryPreferred(secondaryPreferred.toReadPrefOps());
    doc->setNearest(nearest.toReadPrefOps());
    doc->setTagged(tagged.toReadPrefOps());
}

void ReadPreferenceMetrics::generateMetricsDoc(ReadPreferenceMetricsDoc* doc) {
    ReadPrefDoc primary;
    primaryCounters.flushCounters(&primary);
    doc->setExecutedOnPrimary(primary);

    ReadPrefDoc secondary;
    secondaryCounters.flushCounters(&secondary);
    doc->setExecutedOnSecondary(secondary);
}

namespace {
class ReadPreferenceMetricsSSS : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    ~ReadPreferenceMetricsSSS() override = default;

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        if (!repl::ReplicationCoordinator::get(opCtx)->getSettings().isReplSet()) {
            return BSONObj();
        }
        ReadPreferenceMetricsDoc stats;
        ReadPreferenceMetrics::get(opCtx)->generateMetricsDoc(&stats);
        return stats.toBSON();
    }
};
auto& readPreferenceMetricsSSS =
    *ServerStatusSectionBuilder<ReadPreferenceMetricsSSS>("readPreferenceCounters");
}  // namespace

}  // namespace mongo
