// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/commands/server_status/server_status.h"
#include "mongo/db/memory_tracking/query_memory_load_shedding.h"

namespace mongo {
namespace {

// serverStatus().queryMemory -- query-memory metrics. Holds the load-shedding decision state
// (current RSS usage, the water marks, the memory limit, and the shed count) when load shedding is
// enabled; the "queryMemory" parent leaves room for future query memory-tracking metrics.
class QueryMemoryServerStatusSection : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        BSONObjBuilder builder;
        appendQueryMemoryLoadSheddingStats(opCtx->getServiceContext(), builder);
        return builder.obj();
    }
};

auto& queryMemorySection =
    *ServerStatusSectionBuilder<QueryMemoryServerStatusSection>("queryMemory")
         .forShard()
         .forRouter();

}  // namespace
}  // namespace mongo
