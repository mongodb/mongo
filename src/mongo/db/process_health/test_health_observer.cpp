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

#include "mongo/db/process_health/test_health_observer.h"
#include "mongo/db/process_health/health_observer_registration.h"

namespace mongo {
namespace process_health {
MONGO_FAIL_POINT_DEFINE(hangTestHealthObserver);
MONGO_FAIL_POINT_DEFINE(testHealthObserver);
Future<HealthCheckStatus> TestHealthObserver::periodicCheckImpl(
    PeriodicHealthCheckContext&& periodicCheckContext) {
    hangTestHealthObserver.pauseWhileSet();

    auto result = Future<HealthCheckStatus>::makeReady(makeHealthyStatus());

    testHealthObserver.executeIf(
        [this, &result](const BSONObj& data) {
            auto code = data["code"].checkAndGetStringData();
            auto msg = data["msg"].checkAndGetStringData();
            result = Future<HealthCheckStatus>::makeReady(makeSimpleFailedStatus(
                1.0, {Status(ErrorCodes::fromString(code.toString()), msg.toString())}));
        },
        [&](const BSONObj& data) { return !data.isEmpty(); });

    return result;
}

namespace {
MONGO_INITIALIZER(TestHealthObserver)(InitializerContext*) {
    HealthObserverRegistration::registerObserverFactory(
        [](ServiceContext* svcCtx) { return std::make_unique<TestHealthObserver>(svcCtx); });
}
}  // namespace
}  // namespace process_health
}  // namespace mongo
