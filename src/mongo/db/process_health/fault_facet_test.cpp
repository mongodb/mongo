// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/process_health/fault_facet.h"

#include "mongo/db/process_health/fault_facet_impl.h"
#include "mongo/db/process_health/fault_facet_mock.h"
#include "mongo/db/service_context.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/clock_source_mock.h"

#include <string_view>

namespace mongo {
namespace process_health {

namespace {
using namespace std::literals::string_view_literals;

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
    ASSERT_EQ(status.getShortDescription(), "test"sv);
}

TEST_F(FaultFacetImplTest, Update) {
    createWithStatus(kFailure);
    update(kNoFailures);
    ASSERT_EQ(getStatus().getSeverity(), Severity::kOk);
}

}  // namespace
}  // namespace process_health
}  // namespace mongo
