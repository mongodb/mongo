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

#include "mongo/db/process_health/fault_impl.h"

#include "mongo/db/process_health/fault_facet_mock.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"

namespace mongo {
namespace process_health {
namespace {

class FaultImplTest : public unittest::Test {
public:
    void setUp() override {
        _svcCtx = ServiceContext::make();
        _svcCtx->setFastClockSource(std::make_unique<ClockSourceMock>());
        _faultImpl = std::make_unique<FaultImpl>(_svcCtx.get());
    }

    ClockSourceMock& clockSource() {
        return *static_cast<ClockSourceMock*>(_svcCtx->getFastClockSource());
    }

    FaultImpl& fault() {
        return *_faultImpl;
    }

    ServiceContext* svcCtx() {
        return _svcCtx.get();
    }

private:
    ServiceContext::UniqueServiceContext _svcCtx;

    std::unique_ptr<FaultImpl> _faultImpl;
};


TEST_F(FaultImplTest, TimeSourceWorks) {
    // Fault was just created, duration should be zero.
    ASSERT_EQ(Milliseconds(0), fault().getDuration());
    clockSource().advance(Milliseconds(1));
    ASSERT_EQ(Milliseconds(1), fault().getDuration());
}

TEST_F(FaultImplTest, SeverityLevelHelpersWork) {
    FaultFacetMock resolvedFacet(svcCtx(), [](double* severity) { *severity = 0; });
    ASSERT_TRUE(HealthCheckStatus::isResolved(resolvedFacet.getStatus().getSeverity()));

    FaultFacetMock transientFacet(svcCtx(), [](double* severity) { *severity = 0.99; });
    ASSERT_TRUE(HealthCheckStatus::isTransientFault(transientFacet.getStatus().getSeverity()));

    FaultFacetMock faultyFacet(svcCtx(), [](double* severity) { *severity = 1.0; });
    ASSERT_TRUE(HealthCheckStatus::isActiveFault(faultyFacet.getStatus().getSeverity()));
}

}  // namespace
}  // namespace process_health
}  // namespace mongo
