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

#include "mongo/db/process_health/fault_facet.h"

#include "mongo/base/string_data.h"
#include "mongo/db/process_health/fault_facet_impl.h"
#include "mongo/db/process_health/fault_facet_mock.h"
#include "mongo/db/service_context.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/clock_source_mock.h"

namespace mongo {
namespace process_health {

namespace {

// Using the Mock facet.
class FaultFacetTestWithMock : public unittest::Test {
public:
    void startMock(FaultFacetMock::MockCallback callback) {
        _svcCtx = ServiceContext::make(std::make_unique<ClockSourceMock>());
        _facetMock = std::make_unique<FaultFacetMock>(
            FaultFacetType::kMock1, _svcCtx->getFastClockSource(), callback);
    }

    void tearDown() override {
        serverGlobalParams.clusterRole = _saved;
    }

    HealthCheckStatus getStatus() const {
        return _facetMock->getStatus();
    }

private:
    ServiceContext::UniqueServiceContext _svcCtx;
    std::unique_ptr<FaultFacetMock> _facetMock;
    ClusterRole _saved{std::exchange(serverGlobalParams.clusterRole, ClusterRole::RouterServer)};
};

TEST_F(FaultFacetTestWithMock, FacetWithFailure) {
    startMock([] { return Severity::kFailure; });
    ASSERT_EQUALS(Severity::kFailure, getStatus().getSeverity());
}

// Using the FaultFacetImpl.
class FaultFacetImplTest : public unittest::Test {
public:
    static inline const auto kNoFailures =
        HealthCheckStatus(FaultFacetType::kMock1, Severity::kOk, "test");
    static inline const auto kFailure =
        HealthCheckStatus(FaultFacetType::kMock1, Severity::kFailure, "test");

    void setUp() override {
        _svcCtx = ServiceContext::make(std::make_unique<ClockSourceMock>());
    }

    void tearDown() override {
        serverGlobalParams.clusterRole = _saved;
    }

    void createWithStatus(HealthCheckStatus status) {
        _facet = std::make_unique<FaultFacetImpl>(
            FaultFacetType::kMock1, _svcCtx->getFastClockSource(), status);
    }

    void update(HealthCheckStatus status) {
        _facet->update(status);
    }

    HealthCheckStatus getStatus() const {
        return _facet->getStatus();
    }

    ClockSourceMock& clockSource() {
        return *static_cast<ClockSourceMock*>(_svcCtx->getFastClockSource());
    }

    template <typename Duration>
    void advanceTime(Duration d) {
        clockSource().advance(d);
    }

private:
    ServiceContext::UniqueServiceContext _svcCtx;
    std::unique_ptr<FaultFacetImpl> _facet;
    ClusterRole _saved{std::exchange(serverGlobalParams.clusterRole, ClusterRole::RouterServer)};
};

TEST_F(FaultFacetImplTest, Simple) {
    createWithStatus(kFailure);
    const auto status = getStatus();
    ASSERT_GT(status.getSeverity(), Severity::kOk);
    ASSERT_EQ(status.getType(), FaultFacetType::kMock1);
    ASSERT_EQ(status.getShortDescription(), "test"_sd);
}

TEST_F(FaultFacetImplTest, Update) {
    createWithStatus(kFailure);
    update(kNoFailures);
    ASSERT_EQ(getStatus().getSeverity(), Severity::kOk);
}

}  // namespace
}  // namespace process_health
}  // namespace mongo
