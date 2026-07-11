// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/process_health/fault.h"

#include "mongo/db/process_health/fault_facet_mock.h"
#include "mongo/db/process_health/health_check_status.h"
#include "mongo/db/service_context.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/synchronized_value.h"

#include <utility>

namespace mongo {
namespace process_health {
namespace {

class FaultTest : public unittest::Test {
public:
    void setUp() override {
        _svcCtx = ServiceContext::make(std::make_unique<ClockSourceMock>());
        _faultImpl = std::make_unique<Fault>(_svcCtx->getFastClockSource());
    }

    ClockSourceMock& clockSource() {
        return *static_cast<ClockSourceMock*>(_svcCtx->getFastClockSource());
    }

    Fault& fault() {
        return *_faultImpl;
    }

private:
    ServiceContext::UniqueServiceContext _svcCtx;

    std::unique_ptr<Fault> _faultImpl;
};


TEST_F(FaultTest, TimeSourceWorks) {
    // Fault was just created, duration should be zero.
    ASSERT_EQ(Milliseconds(0), fault().getDuration());
    clockSource().advance(Milliseconds(1));
    ASSERT_EQ(Milliseconds(1), fault().getDuration());
}

TEST_F(FaultTest, SeverityLevelHelpersWork) {
    FaultFacetMock resolvedFacet(
        FaultFacetType::kMock1, &clockSource(), [] { return Severity::kOk; });
    ASSERT_TRUE(HealthCheckStatus::isResolved(resolvedFacet.getStatus().getSeverity()));

    FaultFacetMock faultyFacet(
        FaultFacetType::kMock1, &clockSource(), [] { return Severity::kFailure; });
    ASSERT_TRUE(HealthCheckStatus::isActiveFault(faultyFacet.getStatus().getSeverity()));
}

TEST_F(FaultTest, FindFacetByType) {
    ASSERT_EQ(0, fault().getFacets().size());
    ASSERT_FALSE(fault().getFaultFacet(FaultFacetType::kMock1));

    FaultFacetPtr newFacet = std::make_shared<FaultFacetMock>(
        FaultFacetType::kMock1, &clockSource(), [] { return Severity::kOk; });
    fault().upsertFacet(newFacet);
    auto facet = fault().getFaultFacet(FaultFacetType::kMock1);
    ASSERT_TRUE(facet);
    auto status = facet->getStatus();
    ASSERT_EQ(FaultFacetType::kMock1, status.getType());
}

TEST_F(FaultTest, CanCreateAndGarbageCollectFacets) {
    auto severity = synchronized_value<Severity>(Severity::kFailure);

    ASSERT_EQ(0, fault().getFacets().size());
    FaultFacetPtr newFacet = std::make_shared<FaultFacetMock>(
        FaultFacetType::kMock1, &clockSource(), [&severity] { return *severity; });
    fault().upsertFacet(newFacet);
    // New facet was added successfully.
    ASSERT_EQ(1, fault().getFacets().size());

    // Facet cannot be garbage collected.
    fault().garbageCollectResolvedFacets();
    ASSERT_EQ(1, fault().getFacets().size());

    *severity = Severity::kOk;
    fault().garbageCollectResolvedFacets();
    ASSERT_EQ(0, fault().getFacets().size());
}

}  // namespace
}  // namespace process_health
}  // namespace mongo
