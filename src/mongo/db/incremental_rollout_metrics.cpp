// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/commands/server_status/server_status.h"
#include "mongo/db/feature_flag.h"

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;
class IncrementalRolloutServerStatusSection : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        BSONObjBuilder builder;
        BSONArrayBuilder arrayBuilder(builder.subarrayStart("featureFlags"sv));
        IncrementalRolloutFeatureFlag::appendFlagsStats(arrayBuilder);

        arrayBuilder.doneFast();
        return builder.obj();
    }
};

auto& incrementalRolloutSection =
    *ServerStatusSectionBuilder<IncrementalRolloutServerStatusSection>("incrementalRollout")
         .forShard()
         .forRouter();

// Surfaces the process-wide IFR wire-protocol counters (see feature_flag.cpp). Distinct from the
// per-flag "incrementalRollout" section above: these are aggregate cluster-health signals for the
// IFR wire protocol.
class IfrServerStatusSection : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        BSONObjBuilder builder;
        // Count of inbound requests that carried an ifrFlags wire payload (one per fromWire
        // construction — a single client command targeting N shards produces N increments).
        builder.append("cumulativeWireInstalls",
                       IncrementalRolloutFeatureFlag::getWireInstallsCount());
        // Count of IFR flags silently dropped because they were unknown to this binary and the
        // sender is newer.
        builder.append("unknownWireFlagsDropped",
                       IncrementalRolloutFeatureFlag::getUnknownWireFlagsDroppedCount());
        // Count of protocol errors where one or more unknown IFR flags arrived from a
        // same-or-older sender. Non-zero indicates a misconfiguration.
        builder.append("unknownWireFlagErrors",
                       IncrementalRolloutFeatureFlag::getUnknownWireFlagErrorsCount());
        // Count of active flags absent from an inbound wire payload and conservatively resolved to
        // false because the sender predates the flag's introduction.
        builder.append("absentFlagsConservativeFalse",
                       IncrementalRolloutFeatureFlag::getAbsentFlagsConservativeFalseCount());
        // Count of active flags absent from an inbound wire payload and resolved to this binary's
        // local default because the sender is same-or-newer.
        builder.append("absentFlagsLocalDefault",
                       IncrementalRolloutFeatureFlag::getAbsentFlagsLocalDefaultCount());
        return builder.obj();
    }
};

auto& ifrSection =
    *ServerStatusSectionBuilder<IfrServerStatusSection>("ifr").forShard().forRouter();
}  // namespace
}  // namespace mongo
