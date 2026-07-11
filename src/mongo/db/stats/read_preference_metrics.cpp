// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/stats/read_preference_metrics.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/commands/server_status/server_status.h"
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
    if (!readPref.tags.isPrimaryOnly() && !readPref.tags.isMatchAnyNode()) {
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
    *ServerStatusSectionBuilder<ReadPreferenceMetricsSSS>("readPreferenceCounters").forShard();
}  // namespace

}  // namespace mongo
