/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/stats/api_version_metrics.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace {

class APIVersionMetricsTest : public ServiceContextTest {

public:
    virtual void setUp() {
        apiParams = APIParameters();

        // The fast clock is used by OperationContext::hasDeadlineExpired.
        getServiceContext()->setFastClockSource(
            std::make_unique<SharedClockSourceAdapter>(_clkSource));
        // The precise clock is used by waitForConditionOrInterruptNoAssertUntil.
        getServiceContext()->setPreciseClockSource(
            std::make_unique<SharedClockSourceAdapter>(_clkSource));
    }

    virtual void tearDown() {}

protected:
    /**
     * Returns the APIVersionMetrics.
     */
    APIVersionMetrics& getMetrics() {
        return APIVersionMetrics::get(getServiceContext());
    }

    /**
     * Advance the time by millis on both clock source mocks.
     */
    void advanceTime(Milliseconds millis) {
        _clkSource->advance(millis);
    }

    void assertShouldExistInMap(APIVersionMetrics::APIVersionMetricsMap metricsMap,
                                std::string target,
                                bool shouldExist = true) {
        auto metricsIter = metricsMap.find(appName);
        ASSERT(metricsIter != metricsMap.end());

        auto versionTimestampMap = metricsIter->second;
        auto versionTimestampIter = versionTimestampMap.find(target);
        bool existsInMap = (versionTimestampIter != versionTimestampMap.end());
        ASSERT_EQ(shouldExist, existsInMap);
    }

    const std::string appName = "apiVersionMetricsTest";
    APIParameters apiParams;

private:
    std::shared_ptr<ClockSourceMock> _clkSource = std::make_shared<ClockSourceMock>();
};

TEST_F(APIVersionMetricsTest, StoresDefaultMetrics) {
    ASSERT_FALSE(apiParams.getParamsPassed());

    getMetrics().update(appName, apiParams);
    auto metricsMap = getMetrics().getAPIVersionMetrics_forTest();

    // Verify that the metric was inserted with API version set to 'default'.
    assertShouldExistInMap(metricsMap, "default");
}

TEST_F(APIVersionMetricsTest, StoresNonDefaultMetrics) {
    apiParams.setParamsPassed(true);
    ASSERT_TRUE(apiParams.getParamsPassed());

    getMetrics().update(appName, apiParams);
    auto metricsMap = getMetrics().getAPIVersionMetrics_forTest();

    // Verify that the metric was inserted with API version set to '1'.
    assertShouldExistInMap(metricsMap, "1");
}

TEST_F(APIVersionMetricsTest, RemovesStaleMetrics) {
    // Insert a timestamp with default version.
    getMetrics().update(appName, apiParams);

    // Verify that the default metric was inserted correctly.
    auto metricsMap = getMetrics().getAPIVersionMetrics_forTest();
    assertShouldExistInMap(metricsMap, "default");
    assertShouldExistInMap(metricsMap, "1", false /* shouldExist */);

    // Advance the clock by more than a half day.
    auto timeToAdvance = Milliseconds(1005 * 60 * 60 * 12);
    advanceTime(timeToAdvance);

    // Insert a timestamp with API version 1.
    apiParams.setParamsPassed(true);
    getMetrics().update(appName, apiParams);

    // Verify that both metrics are still within the map.
    metricsMap = getMetrics().getAPIVersionMetrics_forTest();
    assertShouldExistInMap(metricsMap, "default");
    assertShouldExistInMap(metricsMap, "1");

    // Advance the clock by more than a half day.
    timeToAdvance = Milliseconds(1005 * 60 * 60 * 12);
    advanceTime(timeToAdvance);

    // Verify that the default metric was removed, but the metric with API version 1 was not.
    metricsMap = getMetrics().getAPIVersionMetrics_forTest();
    assertShouldExistInMap(metricsMap, "default", false /* shouldExist */);
    assertShouldExistInMap(metricsMap, "1");

    // Advance the clock by more than a half day.
    timeToAdvance = Milliseconds(1005 * 60 * 60 * 12);
    advanceTime(timeToAdvance);

    // Verify that both metrics were correctly removed.
    metricsMap = getMetrics().getAPIVersionMetrics_forTest();
    ASSERT(metricsMap.find(appName) == metricsMap.end());
}

}  // namespace
}  // namespace mongo
