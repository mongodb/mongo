// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/commands/server_status/server_status.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/stats/top.h"

#include <memory>

namespace mongo {
namespace {
/**
 * Appends the global histogram to the server status.
 */
class GlobalHistogramServerStatusSection final : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx, const BSONElement& configElem) const override {
        BSONObjBuilder latencyBuilder;
        bool includeHistograms = false;
        bool slowBuckets = false;
        if (configElem.type() == BSONType::object) {
            includeHistograms = configElem.Obj()["histograms"].trueValue();
            slowBuckets = configElem.Obj()["slowBuckets"].trueValue();
        }
        ServiceLatencyTracker::getDecoration(opCtx->getService())
            .appendTotalTimeStats(includeHistograms, slowBuckets, &latencyBuilder);
        return latencyBuilder.obj();
    }
};

class WorkingTimeHistogramServerStatusSection final : public ServerStatusSection {
    using ServerStatusSection::ServerStatusSection;

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx, const BSONElement& configElem) const override {
        BSONObjBuilder latencyBuilder;
        bool includeHistograms = false;
        bool slowBuckets = false;
        if (configElem.type() == BSONType::object) {
            includeHistograms = configElem.Obj()["histograms"].trueValue();
            slowBuckets = configElem.Obj()["slowBuckets"].trueValue();
        }
        ServiceLatencyTracker::getDecoration(opCtx->getService())
            .appendWorkingTimeStats(includeHistograms, slowBuckets, &latencyBuilder);
        return latencyBuilder.obj();
    }
};
auto globalHistogramServerStatusSection =
    *ServerStatusSectionBuilder<GlobalHistogramServerStatusSection>("opLatencies")
         .forRouter()
         .forShard();

auto globalWorkingTimeHistogramServerStatusSection =
    *ServerStatusSectionBuilder<WorkingTimeHistogramServerStatusSection>("opWorkingTime")
         .forRouter()
         .forShard();
}  // namespace
}  // namespace mongo
