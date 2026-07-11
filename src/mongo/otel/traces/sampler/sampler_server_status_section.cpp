// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands/server_status/server_status.h"
#include "mongo/otel/traces/sampler/sampler.h"

namespace mongo::otel::traces {
namespace {

class TracingSamplerServerStatusSection : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        const auto stats = TracingSampler::get().getStats();
        BSONObjBuilder b;
        {
            BSONObjBuilder bob(b.subobjStart("internalSpans"));
            bob.append("acceptedByRateLimiter", stats.internalSpans.admitted);
            bob.append("rejectedByRateLimiter", stats.internalSpans.rejected);
        }
        {
            BSONObjBuilder bob(b.subobjStart("externalSpan"));
            bob.append("acceptedByRateLimiter", stats.externalSpan.admitted);
            bob.append("rejectedByRateLimiter", stats.externalSpan.rejected);
        }
        return b.obj();
    }
};

auto& tracingSamplerSection =
    *ServerStatusSectionBuilder<TracingSamplerServerStatusSection>("otelTracingSampler")
         .forShard()
         .forRouter();

}  // namespace
}  // namespace mongo::otel::traces
