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

#include "mongo/db/process_health/fault.h"

#include "mongo/base/string_data.h"
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
