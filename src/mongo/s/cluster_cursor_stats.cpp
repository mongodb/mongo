/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <memory>
#include <string>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/service_context.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/exec/cluster_cursor_manager.h"

namespace mongo {
namespace {

/** ServerStatus metric cursor counts. */
struct CursorStatsMetricPolicy {
    void appendTo(BSONObjBuilder& b, StringData leafName) const {
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
