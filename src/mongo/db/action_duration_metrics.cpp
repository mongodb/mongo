/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/action_duration_metrics.h"

#include "mongo/db/commands/server_status/server_status.h"

namespace mongo {

namespace {

const auto getActionDurationMetrics = ServiceContext::declareDecoration<ActionDurationMetrics>();

class ActionDurationServerStatus : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    bool includeByDefault() const final {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx, const BSONElement& configElement) const final {
        BSONObjBuilder b;
        getActionDurationMetrics(opCtx->getServiceContext()).report(&b);
        return b.obj();
    }
};

auto& actionDurationServerStatus =
    *ServerStatusSectionBuilder<ActionDurationServerStatus>("actionDuration")
         .forShard()
         .forRouter();

}  // namespace

const ActionDurationMetrics& ActionDurationMetrics::getDecoration(ServiceContext* serviceContext) {
    return getActionDurationMetrics(serviceContext);
}

void ActionDurationMetrics::record(const std::string& action, Milliseconds millis) {
    stdx::lock_guard lk(_mutex);
    auto& entry = _entries[action];
    entry.last = millis;
    entry.total += millis;
    entry.count++;
}

void ActionDurationMetrics::report(BSONObjBuilder* builder) const {
    stdx::lock_guard lk(_mutex);
    for (const auto& [action, entry] : _entries) {
        BSONObjBuilder sub(builder->subobjStart(action));
        sub.append("lastDurationMillis", durationCount<Milliseconds>(entry.last));
        sub.append("totalDurationMillis", durationCount<Milliseconds>(entry.total));
        sub.append("count", entry.count);
    }
}

ActionDurationTimer::ActionDurationTimer(OperationContext* opCtx, std::string action)
    : _opCtx(opCtx), _action(std::move(action)), _start(opCtx->fastClockSource().now()) {}

ActionDurationTimer::~ActionDurationTimer() {
    Milliseconds duration = _opCtx->fastClockSource().now() - _start;
    getActionDurationMetrics(_opCtx->getServiceContext()).record(_action, duration);
}

}  // namespace mongo
