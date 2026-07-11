// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/process_health/test_health_observer.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/process_health/health_observer.h"
#include "mongo/db/process_health/health_observer_registration.h"
#include "mongo/logv2/log.h"
#include "mongo/util/fail_point.h"

#include <memory>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kProcessHealth


namespace mongo {
namespace process_health {
MONGO_FAIL_POINT_DEFINE(hangTestHealthObserver);
MONGO_FAIL_POINT_DEFINE(testHealthObserver);
MONGO_FAIL_POINT_DEFINE(badConfigTestHealthObserver);
MONGO_FAIL_POINT_DEFINE(statusFailureTestHealthObserver);
Future<HealthCheckStatus> TestHealthObserver::periodicCheckImpl(
    PeriodicHealthCheckContext&& periodicCheckContext) {
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
                {Status(ErrorCodes::fromString(std::string{code}), std::string{msg})}));
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
