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
}  // namespace
}  // namespace mongo
