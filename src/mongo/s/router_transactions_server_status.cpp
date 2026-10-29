// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/commands/server_status/server_status.h"
#include "mongo/db/operation_context.h"
#include "mongo/s/router_transactions_metrics.h"
#include "mongo/s/router_transactions_stats_gen.h"

#include <memory>

namespace mongo {
namespace {

class RouterTransactionsSSS final : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        RouterTransactionsStats stats;
        RouterTransactionsMetrics::get(opCtx)->updateStats(&stats);
        return stats.toBSON();
    }
};
auto& routerTransactionsSSS =
    *ServerStatusSectionBuilder<RouterTransactionsSSS>("transactions").forRouter();

}  // namespace
}  // namespace mongo
