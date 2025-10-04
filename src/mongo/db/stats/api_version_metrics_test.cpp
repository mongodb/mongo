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

#include "mongo/db/stats/api_version_metrics.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/duration.h"

#include <memory>
#include <set>

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>

namespace mongo {

using VersionMetricsMap = APIVersionMetrics::VersionMetricsMap;
namespace {

class APIVersionMetricsTest : public SharedClockSourceAdapterServiceContextTest {
    explicit APIVersionMetricsTest(std::shared_ptr<ClockSourceMock> mockClock)
        : SharedClockSourceAdapterServiceContextTest(mockClock), _clkSource(std::move(mockClock)) {}

public:
    void setUp() override {
        apiParams = APIParameters();
    }

    void tearDown() override {}

protected:
    APIVersionMetricsTest() : APIVersionMetricsTest(std::make_shared<ClockSourceMock>()) {}

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

    void assertShouldExistInMap(const VersionMetricsMap& metricsMap,
                                std::string appName,
                                int target,
                                bool shouldExist = true) {
        auto metricsIter = metricsMap.find(appName);
        ASSERT(metricsIter != metricsMap.end());

        const auto& versionTimestampMap = metricsIter->second;
        const auto& timestamp = versionTimestampMap.timestamps[target];
        bool existsInMap = (timestamp.loadRelaxed() != Date_t::min());
        ASSERT_EQ(shouldExist, existsInMap);
    }

    const std::string appName = "apiVersionMetricsTest";
    APIParameters apiParams;

private:
    std::shared_ptr<ClockSourceMock> _clkSource;
};

TEST_F(APIVersionMetricsTest, StoresDefaultMetrics) {
    ASSERT_FALSE(apiParams.getParamsPassed());

    getMetrics().update(appName, apiParams);
    VersionMetricsMap map;
    getMetrics().cloneAPIVersionMetrics_forTest(map);

    // Verify that the metric was inserted with API version set to 'default'.
    assertShouldExistInMap(map, appName, APIVersionMetrics::kVersionMetricsDefaultVersionPosition);
}

TEST_F(APIVersionMetricsTest, StoresNonDefaultMetrics) {
    apiParams.setAPIVersion("1");
    ASSERT_TRUE(apiParams.getParamsPassed());

    getMetrics().update(appName, apiParams);
    VersionMetricsMap map;
    getMetrics().cloneAPIVersionMetrics_forTest(map);

    // Verify that the metric was inserted with API version set to '1'.
    assertShouldExistInMap(map, appName, APIVersionMetrics::kVersionMetricsVersionOnePosition);
}

TEST_F(APIVersionMetricsTest, RemovesStaleMetrics) {
    // Insert a timestamp with default version.
    getMetrics().update(appName, apiParams);

    // Verify that the default metric was inserted correctly.
    VersionMetricsMap map;
    getMetrics().cloneAPIVersionMetrics_forTest(map);
    assertShouldExistInMap(map, appName, APIVersionMetrics::kVersionMetricsDefaultVersionPosition);
    assertShouldExistInMap(map,
                           appName,
                           APIVersionMetrics::kVersionMetricsVersionOnePosition,
                           false /* shouldExist */);

    // Advance the clock by more than a half day.
    auto timeToAdvance = Milliseconds(1005 * 60 * 60 * 12);
    advanceTime(timeToAdvance);

    // Insert a timestamp with API version 1.
    apiParams.setAPIVersion("1");
    getMetrics().update(appName, apiParams);

    // Verify that both metrics are still within the map.
    getMetrics().cloneAPIVersionMetrics_forTest(map);
    assertShouldExistInMap(map, appName, APIVersionMetrics::kVersionMetricsDefaultVersionPosition);
    assertShouldExistInMap(map, appName, APIVersionMetrics::kVersionMetricsVersionOnePosition);

    // Advance the clock by more than a half day.
    timeToAdvance = Milliseconds(1005 * 60 * 60 * 12);
    advanceTime(timeToAdvance);

    // Both metrics remain in the map since we don't modify until all versions for an appName are
    // stale.
    getMetrics().cloneAPIVersionMetrics_forTest(map);
    assertShouldExistInMap(map, appName, APIVersionMetrics::kVersionMetricsDefaultVersionPosition);
    assertShouldExistInMap(map, appName, APIVersionMetrics::kVersionMetricsVersionOnePosition);

    // Advance the clock by more than a half day.
    timeToAdvance = Milliseconds(1005 * 60 * 60 * 12);
    advanceTime(timeToAdvance);

    // Verify that both metrics were correctly removed.
    getMetrics().cloneAPIVersionMetrics_forTest(map);
    ASSERT(map.find(appName) == map.end());
}

TEST_F(APIVersionMetricsTest, TestOutputBSONSizeLimit) {
    apiParams.setAPIVersion("1");
    ASSERT_TRUE(apiParams.getParamsPassed());
    APIParameters defaultApiParams;
    ASSERT_FALSE(defaultApiParams.getParamsPassed());
    // Note that an additional entry will be added to the data, but it will not be included in the
    // output that is displayed.
    for (auto i = -1; i < APIVersionMetrics::kMaxNumOfOutputAppNames; i++) {
        auto appNameStr = appName + ((i > -1) ? "_" + std::to_string(i) : "");
        getMetrics().update(appNameStr, defaultApiParams);
        getMetrics().update(appNameStr, apiParams);
    }

    // Verify that the metric was inserted correctly.
    VersionMetricsMap map;
    getMetrics().cloneAPIVersionMetrics_forTest(map);
    for (auto i = -1; i < APIVersionMetrics::kMaxNumOfOutputAppNames; i++) {
        auto appNameStr = appName + ((i > -1) ? "_" + std::to_string(i) : "");
        assertShouldExistInMap(
            map, appNameStr, APIVersionMetrics::kVersionMetricsVersionOnePosition);
        assertShouldExistInMap(
            map, appNameStr, APIVersionMetrics::kVersionMetricsDefaultVersionPosition);
    }

    // Verify that output is capped.
    BSONObjBuilder bob;
    getMetrics().appendAPIVersionMetricsInfo_forTest(&bob);
    auto outputObj = bob.obj();
    int notInOutput = 0;
    for (auto i = -1; i < APIVersionMetrics::kMaxNumOfOutputAppNames; i++) {
        auto appNameStr = appName + ((i > -1) ? "_" + std::to_string(i) : "");

        if (!outputObj.hasField(appNameStr)) {
            notInOutput++;
        } else {
            // Verify that output is correct.
            BSONObj sub = outputObj.getObjectField(appNameStr);
            std::set<std::string> apiVersions;
            for (const auto& element : sub) {
                apiVersions.insert(element.str());
            }
            ASSERT_TRUE(apiVersions.size() == 2);
            ASSERT_TRUE(apiVersions.count("default"));
            ASSERT_TRUE(apiVersions.count("1"));
        }

        ASSERT_TRUE(notInOutput < 2);
    }
}

TEST_F(APIVersionMetricsTest, TestSavedAppNamesLimit) {
    // Note that an additional entry will be added to the data, but it will not be included in the
    // output that is displayed.
    apiParams.setAPIVersion("1");
    ASSERT_TRUE(apiParams.getParamsPassed());
    APIParameters defaultApiParams;
    ASSERT_FALSE(defaultApiParams.getParamsPassed());
    for (auto i = 0; i < APIVersionMetrics::kMaxNumOfSavedAppNames; i++) {
        auto appNameStr = appName + "_" + std::to_string(i);
        getMetrics().update(appNameStr, defaultApiParams);
    }

    // Verify that the metric was inserted correctly.
    VersionMetricsMap map;
    getMetrics().cloneAPIVersionMetrics_forTest(map);
    for (auto i = 0; i < APIVersionMetrics::kMaxNumOfSavedAppNames; i++) {
        auto appNameStr = appName + "_" + std::to_string(i);
        assertShouldExistInMap(
            map, appNameStr, APIVersionMetrics::kVersionMetricsDefaultVersionPosition);
    }

    // Attempting to add another entry beyond the limit will not succeed.
    getMetrics().update(appName, defaultApiParams);
    getMetrics().cloneAPIVersionMetrics_forTest(map);
    auto metricsIter = map.find(appName);
    ASSERT(metricsIter == map.end());

    // Modifying existing elements is permitted.
    for (auto i = 0; i < APIVersionMetrics::kMaxNumOfSavedAppNames; i++) {
        auto appNameStr = appName + "_" + std::to_string(i);
        getMetrics().update(appNameStr, apiParams);
    }

    // Verify that the metric got updated.
    getMetrics().cloneAPIVersionMetrics_forTest(map);
    for (auto i = 0; i < APIVersionMetrics::kMaxNumOfSavedAppNames; i++) {
        auto appNameStr = appName + "_" + std::to_string(i);
        assertShouldExistInMap(
            map, appNameStr, APIVersionMetrics::kVersionMetricsDefaultVersionPosition);
        assertShouldExistInMap(
            map, appNameStr, APIVersionMetrics::kVersionMetricsVersionOnePosition);
    }
}

}  // namespace
}  // namespace mongo
