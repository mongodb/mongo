// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands/server_status/server_status_metric.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/s/query/exec/cluster_cursor_manager.h"

#include <string>
#include <string_view>

namespace mongo {
namespace {

/** ServerStatus metric cursor counts. */
struct CursorStatsMetricPolicy {
    void appendTo(BSONObjBuilder& b, std::string_view leafName) const {
        auto ll = [](auto v) {
            return static_cast<long long>(v);
        };
        auto grid = Grid::get(getGlobalServiceContext());
        BSONObjBuilder cursorBob(b.subobjStart(leafName));
        cursorBob.append("timedOut", ll(grid->getCursorManager()->cursorsTimedOut()));
        auto stats = grid->getCursorManager()->getOpenCursorStats();
        BSONObjBuilder{cursorBob.subobjStart("open")}
            .append("multiTarget", ll(stats.multiTarget))
            .append("singleTarget", ll(stats.singleTarget))
            .append("queuedData", ll(stats.queuedData))
            .append("pinned", ll(stats.pinned))
            .append("total", ll(stats.multiTarget + stats.singleTarget));
    }
};

auto& clusterCursorStats =
    *CustomMetricBuilder<CursorStatsMetricPolicy>{"cursor"}.setRole(ClusterRole::RouterServer);

}  // namespace
}  // namespace mongo
