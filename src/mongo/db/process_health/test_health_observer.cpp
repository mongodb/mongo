/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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
#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kProcessHealth

#include "mongo/db/process_health/test_health_observer.h"

#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/process_health/health_observer_registration.h"
#include "mongo/logv2/log.h"

namespace mongo {
namespace process_health {
MONGO_FAIL_POINT_DEFINE(hangTestHealthObserver);
MONGO_FAIL_POINT_DEFINE(testHealthObserver);
MONGO_FAIL_POINT_DEFINE(badConfigTestHealthObserver);
MONGO_FAIL_POINT_DEFINE(statusFailureTestHealthObserver);
Future<HealthCheckStatus> TestHealthObserver::periodicCheckImpl(
    PeriodicHealthCheckContext&& periodicCheckContext) noexcept {
    LOGV2_DEBUG(5936801, 2, "Test health observer executing");
    hangTestHealthObserver.pauseWhileSet();

    if (statusFailureTestHealthObserver.shouldFail()) {
        return Status(ErrorCodes::BadValue, "Status failure in test health observer");
    }

    auto result = Future<HealthCheckStatus>::makeReady(makeHealthyStatus());

    testHealthObserver.executeIf(
        [this, &result](const BSONObj& data) {
            auto code = data["code"].checkAndGetStringData();
            auto msg = data["msg"].checkAndGetStringData();
            result = Future<HealthCheckStatus>::makeReady(makeSimpleFailedStatus(
                Severity::kFailure,
                {Status(ErrorCodes::fromString(code.toString()), msg.toString())}));
        },
        [&](const BSONObj& data) { return !data.isEmpty(); });

    LOGV2_DEBUG(5936802, 2, "Test health observer returns", "result"_attr = result.get());
    return result;
}

bool TestHealthObserver::isConfigured() const {
    if (badConfigTestHealthObserver.shouldFail()) {
        return false;
    }
    return true;
}

namespace {
MONGO_INITIALIZER(TestHealthObserver)(InitializerContext*) {
    // Failpoints can only be set when test commands are enabled, and so the test health observer
    // is only useful in that case.
    if (getTestCommandsEnabled()) {
        LOGV2(5936803, "Test health observer instantiated");
        HealthObserverRegistration::registerObserverFactory(
            [](ServiceContext* svcCtx) { return std::make_unique<TestHealthObserver>(svcCtx); });
    }
}
}  // namespace
}  // namespace process_health
}  // namespace mongo
