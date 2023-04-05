/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/s/query_analysis_coordinator.h"

#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/s/config/config_server_test_fixture.h"
#include "mongo/db/s/query_analysis_op_observer.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/logv2/log.h"
#include "mongo/s/analyze_shard_key_documents_gen.h"
#include "mongo/s/catalog/type_mongos.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/version.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace analyze_shard_key {
namespace {

class QueryAnalysisCoordinatorTest : public ConfigServerTestFixture {
public:
    QueryAnalysisCoordinatorTest() : ConfigServerTestFixture(Options{}.useMockClock(true)) {}

    void setupOpObservers() override {
        ConfigServerTestFixture::setupOpObservers();
        auto opObserverRegistry =
            checked_cast<OpObserverRegistry*>(getServiceContext()->getOpObserver());
        opObserverRegistry->addObserver(std::make_unique<QueryAnalysisOpObserver>());
    }

protected:
    void advanceTime(Seconds secs) {
        _mockClock->advance(secs);
    }

    Date_t now() {
        return _mockClock->now();
    }

    QueryAnalyzerDocument makeConfigQueryAnalyzersDocument(
        const NamespaceString& nss,
        const UUID& collUuid,
        QueryAnalyzerModeEnum mode,
        boost::optional<double> sampleRate = boost::none,
        boost::optional<Date_t> startTime = boost::none,
        boost::optional<Date_t> stopTime = boost::none) {
        QueryAnalyzerDocument doc;
        doc.setNs(nss);
        doc.setCollectionUuid(collUuid);
        QueryAnalyzerConfiguration configuration;
        configuration.setMode(mode);
        configuration.setSampleRate(sampleRate);
        doc.setConfiguration(configuration);
        doc.setStartTime(startTime ? *startTime : now());
        if (mode == QueryAnalyzerModeEnum::kOff) {
            doc.setStopTime(stopTime ? stopTime : now());
        }
        return doc;
    }

    void assertContainsConfiguration(
        const QueryAnalysisCoordinator::CollectionQueryAnalyzerConfigurationMap& configurations,
        QueryAnalyzerDocument analyzerDoc) {
        auto it = configurations.find(analyzerDoc.getCollectionUuid());
        ASSERT(it != configurations.end());
        auto& configuration = it->second;
        ASSERT_EQ(configuration.getNs(), analyzerDoc.getNs());
        ASSERT_EQ(configuration.getCollectionUuid(), analyzerDoc.getCollectionUuid());
        ASSERT_EQ(configuration.getSampleRate(), *analyzerDoc.getSampleRate());
        ASSERT_EQ(configuration.getStartTime(), analyzerDoc.getStartTime());
    }

    void assertContainsConfiguration(
        std::vector<CollectionQueryAnalyzerConfiguration>& configurations,
        const NamespaceString& nss,
        const UUID& collUuid,
        double sampleRate,
        Date_t startTime) {
        for (const auto& configuration : configurations) {
            if (configuration.getNs() == nss) {
                ASSERT_EQ(configuration.getCollectionUuid(), collUuid);
                ASSERT_EQ(configuration.getSampleRate(), sampleRate);
                ASSERT_EQ(configuration.getStartTime(), startTime);
                return;
            }
        }
        FAIL("Cannot find the configuration for the specified collection");
    }

    MongosType makeConfigMongosDocument(std::string name) {
        MongosType doc;
        doc.setName(name);
        doc.setPing(now());
        doc.setUptime(0);
        doc.setWaiting(true);
        doc.setMongoVersion(VersionInfoInterface::instance().version().toString());
        return doc;
    }

    void assertContainsSampler(const StringMap<QueryAnalysisCoordinator::Sampler>& samplers,
                               MongosType mongosDoc) {
        assertContainsSampler(samplers,
                              mongosDoc.getName(),
                              mongosDoc.getPing(),
                              boost::none /*numQueriesPerSecond */);
    }

    void assertContainsSampler(const StringMap<QueryAnalysisCoordinator::Sampler>& samplers,
                               StringData name,
                               Date_t pingTime,
                               boost::optional<double> numQueriesPerSecond) {
        auto it = samplers.find(name);
        ASSERT(it != samplers.end());
        auto& sampler = it->second;
        ASSERT_EQ(sampler.getName(), name);
        ASSERT_EQ(sampler.getLastPingTime(), pingTime);
        if (numQueriesPerSecond) {
            ASSERT(sampler.getLastNumQueriesExecutedPerSecond());
            ASSERT_EQ(sampler.getLastNumQueriesExecutedPerSecond(), numQueriesPerSecond);
        } else {
            ASSERT_FALSE(sampler.getLastNumQueriesExecutedPerSecond());
        }
    }

    const NamespaceString nss0 =
        NamespaceString::createNamespaceString_forTest("testDb", "testColl0");
    const NamespaceString nss1 =
        NamespaceString::createNamespaceString_forTest("testDb", "testColl1");
    const NamespaceString nss2 =
        NamespaceString::createNamespaceString_forTest("testDb", "testColl2");

    const UUID collUuid0 = UUID::gen();
    const UUID collUuid1 = UUID::gen();
    const UUID collUuid2 = UUID::gen();

    const std::string mongosName0 = "FakeHost0:1234";
    const std::string mongosName1 = "FakeHost1:1234";
    const std::string mongosName2 = "FakeHost2:1234";

    int inActiveThresholdSecs = 1000;

private:
    const std::shared_ptr<ClockSourceMock> _mockClock = std::make_shared<ClockSourceMock>();

    RAIIServerParameterControllerForTest _featureFlagController{"featureFlagAnalyzeShardKey", true};
    RAIIServerParameterControllerForTest _inactiveThresholdController{
        "queryAnalysisSamplerInActiveThresholdSecs", inActiveThresholdSecs};
};

TEST_F(QueryAnalysisCoordinatorTest, CreateConfigurationsOnInsert) {
    auto coordinator = QueryAnalysisCoordinator::get(operationContext());

    // There are no configurations initially.
    auto configurations = coordinator->getConfigurationsForTest();
    ASSERT(configurations.empty());

    auto analyzerDoc0 =
        makeConfigQueryAnalyzersDocument(nss0, collUuid0, QueryAnalyzerModeEnum::kFull, 0.5);
    uassertStatusOK(insertToConfigCollection(operationContext(),
                                             NamespaceString::kConfigQueryAnalyzersNamespace,
                                             analyzerDoc0.toBSON()));

    auto analyzerDoc1 =
        makeConfigQueryAnalyzersDocument(nss1, collUuid1, QueryAnalyzerModeEnum::kFull, 5);
    uassertStatusOK(insertToConfigCollection(operationContext(),
                                             NamespaceString::kConfigQueryAnalyzersNamespace,
                                             analyzerDoc1.toBSON()));

    auto analyzerDoc2 =
        makeConfigQueryAnalyzersDocument(nss2, collUuid2, QueryAnalyzerModeEnum::kOff);
    uassertStatusOK(insertToConfigCollection(operationContext(),
                                             NamespaceString::kConfigQueryAnalyzersNamespace,
                                             analyzerDoc2.toBSON()));

    // The inserts should cause the configurations for collection0 and collection1 to get created.
    // There should be no configuration for collection2 since the mode is "off".
    configurations = coordinator->getConfigurationsForTest();
    ASSERT_EQ(configurations.size(), 2U);
    assertContainsConfiguration(configurations, analyzerDoc0);
    assertContainsConfiguration(configurations, analyzerDoc1);
}

TEST_F(QueryAnalysisCoordinatorTest, UpdateConfigurationsOnSampleRateUpdate) {
    auto coordinator = QueryAnalysisCoordinator::get(operationContext());

    // There are no configurations initially.
    auto configurations = coordinator->getConfigurationsForTest();
    ASSERT(configurations.empty());

    auto analyzerDocPreUpdate =
        makeConfigQueryAnalyzersDocument(nss0, collUuid0, QueryAnalyzerModeEnum::kFull, 0.5);
    uassertStatusOK(insertToConfigCollection(operationContext(),
                                             NamespaceString::kConfigQueryAnalyzersNamespace,
                                             analyzerDocPreUpdate.toBSON()));

    configurations = coordinator->getConfigurationsForTest();
    ASSERT_EQ(configurations.size(), 1U);
    assertContainsConfiguration(configurations, analyzerDocPreUpdate);

    auto analyzerDocPostUpdate =
        makeConfigQueryAnalyzersDocument(nss0, collUuid0, QueryAnalyzerModeEnum::kFull, 1.5);
    uassertStatusOK(updateToConfigCollection(operationContext(),
                                             NamespaceString::kConfigQueryAnalyzersNamespace,
                                             BSON("_id" << collUuid0),
                                             analyzerDocPostUpdate.toBSON(),
                                             false /* upsert */));

    // The update should cause the configuration to have the new sample rate.
    configurations = coordinator->getConfigurationsForTest();
    ASSERT_EQ(configurations.size(), 1U);
    assertContainsConfiguration(configurations, analyzerDocPostUpdate);
}

TEST_F(QueryAnalysisCoordinatorTest, UpdateOrRemoveConfigurationsOnModeUpdate) {
    auto coordinator = QueryAnalysisCoordinator::get(operationContext());

    // There are no configurations initially.
    auto configurations = coordinator->getConfigurationsForTest();
    ASSERT(configurations.empty());

    auto analyzerDocPreUpdate =
        makeConfigQueryAnalyzersDocument(nss0, collUuid0, QueryAnalyzerModeEnum::kFull, 0.5);
    uassertStatusOK(insertToConfigCollection(operationContext(),
                                             NamespaceString::kConfigQueryAnalyzersNamespace,
                                             analyzerDocPreUpdate.toBSON()));

    configurations = coordinator->getConfigurationsForTest();
    ASSERT_EQ(configurations.size(), 1U);
    assertContainsConfiguration(configurations, analyzerDocPreUpdate);

    auto analyzerDocPostUpdate0 =
        makeConfigQueryAnalyzersDocument(nss0, collUuid0, QueryAnalyzerModeEnum::kOff);
    uassertStatusOK(updateToConfigCollection(operationContext(),
                                             NamespaceString::kConfigQueryAnalyzersNamespace,
                                             BSON("_id" << collUuid0),
                                             analyzerDocPostUpdate0.toBSON(),
                                             false /* upsert */));

    // The update to mode "off" should cause the configuration to get removed.
    configurations = coordinator->getConfigurationsForTest();
    ASSERT(configurations.empty());

    auto analyzerDocPostUpdate1 =
        makeConfigQueryAnalyzersDocument(nss0, collUuid0, QueryAnalyzerModeEnum::kFull, 15);
    uassertStatusOK(updateToConfigCollection(operationContext(),
                                             NamespaceString::kConfigQueryAnalyzersNamespace,
                                             BSON("_id" << collUuid0),
                                             analyzerDocPostUpdate1.toBSON(),
                                             false /* upsert */));

    // The update to mode "on" should cause the configuration to get recreated.
    configurations = coordinator->getConfigurationsForTest();
    ASSERT_EQ(configurations.size(), 1U);
    assertContainsConfiguration(configurations, analyzerDocPostUpdate1);
}

TEST_F(QueryAnalysisCoordinatorTest, DeleteConfigurationsModeOn) {
    auto coordinator = QueryAnalysisCoordinator::get(operationContext());

    // There are no samplers initially.
    auto configurations = coordinator->getConfigurationsForTest();
    ASSERT(configurations.empty());

    auto analyzerDoc =
        makeConfigQueryAnalyzersDocument(nss0, collUuid0, QueryAnalyzerModeEnum::kFull, 0.5);
    uassertStatusOK(insertToConfigCollection(
        operationContext(), NamespaceString::kConfigQueryAnalyzersNamespace, analyzerDoc.toBSON()));

    configurations = coordinator->getConfigurationsForTest();
    ASSERT_EQ(configurations.size(), 1U);
    assertContainsConfiguration(configurations, analyzerDoc);

    uassertStatusOK(deleteToConfigCollection(operationContext(),
                                             NamespaceString::kConfigQueryAnalyzersNamespace,
                                             analyzerDoc.toBSON(),
                                             false /* multi */));

    // The delete should cause the configuration to get removed.
    configurations = coordinator->getConfigurationsForTest();
    ASSERT(configurations.empty());
}

TEST_F(QueryAnalysisCoordinatorTest, DeleteConfigurationsModeOff) {
    auto coordinator = QueryAnalysisCoordinator::get(operationContext());

    // There are no samplers initially.
    auto configurations = coordinator->getConfigurationsForTest();
    ASSERT(configurations.empty());

    auto analyzerDoc =
        makeConfigQueryAnalyzersDocument(nss0, collUuid0, QueryAnalyzerModeEnum::kOff);
    uassertStatusOK(insertToConfigCollection(
        operationContext(), NamespaceString::kConfigQueryAnalyzersNamespace, analyzerDoc.toBSON()));

    configurations = coordinator->getConfigurationsForTest();
    ASSERT(configurations.empty());

    uassertStatusOK(deleteToConfigCollection(operationContext(),
                                             NamespaceString::kConfigQueryAnalyzersNamespace,
                                             analyzerDoc.toBSON(),
                                             false /* multi */));

    // The delete should be a noop.
    configurations = coordinator->getConfigurationsForTest();
    ASSERT(configurations.empty());
}

TEST_F(QueryAnalysisCoordinatorTest, CreateConfigurationsOnStartUp) {
    auto coordinator = QueryAnalysisCoordinator::get(operationContext());

    // There are no configuration initially.
    auto configurations = coordinator->getConfigurationsForTest();
    ASSERT(configurations.empty());

    // There are no configuration to create upon startup since there are no analyzer documents.
    coordinator->onStartup(operationContext());
    configurations = coordinator->getConfigurationsForTest();
    ASSERT(configurations.empty());

    auto analyzerDoc0 =
        makeConfigQueryAnalyzersDocument(nss0, collUuid0, QueryAnalyzerModeEnum::kFull, 0.5);
    uassertStatusOK(insertToConfigCollection(operationContext(),
                                             NamespaceString::kConfigQueryAnalyzersNamespace,
                                             analyzerDoc0.toBSON()));

    auto analyzerDoc1 =
        makeConfigQueryAnalyzersDocument(nss1, collUuid1, QueryAnalyzerModeEnum::kFull, 5);
    uassertStatusOK(insertToConfigCollection(operationContext(),
                                             NamespaceString::kConfigQueryAnalyzersNamespace,
                                             analyzerDoc1.toBSON()));

    auto analyzerDoc2 =
        makeConfigQueryAnalyzersDocument(nss2, collUuid2, QueryAnalyzerModeEnum::kOff);
    uassertStatusOK(insertToConfigCollection(operationContext(),
                                             NamespaceString::kConfigQueryAnalyzersNamespace,
                                             analyzerDoc2.toBSON()));

    coordinator->clearConfigurationsForTest();
    configurations = coordinator->getConfigurationsForTest();
    ASSERT(configurations.empty());

    // After stepup, the configurations for collection0 and collection1 to get recreated. There
    // should not be a configuration for collection2 since the mode is "off".
    coordinator->onStartup(operationContext());
    configurations = coordinator->getConfigurationsForTest();
    ASSERT_EQ(configurations.size(), 2U);
    assertContainsConfiguration(configurations, analyzerDoc0);
    assertContainsConfiguration(configurations, analyzerDoc1);
}

TEST_F(QueryAnalysisCoordinatorTest, DoesNotClearConfigurationsOnStepUp) {
    auto coordinator = QueryAnalysisCoordinator::get(operationContext());

    // There are no samplers initially.
    auto configurations = coordinator->getConfigurationsForTest();
    ASSERT(configurations.empty());

    auto analyzerDoc =
        makeConfigQueryAnalyzersDocument(nss0, collUuid0, QueryAnalyzerModeEnum::kFull, 0.5);
    uassertStatusOK(insertToConfigCollection(
        operationContext(), NamespaceString::kConfigQueryAnalyzersNamespace, analyzerDoc.toBSON()));

    configurations = coordinator->getConfigurationsForTest();
    ASSERT_EQ(configurations.size(), 1U);
    assertContainsConfiguration(configurations, analyzerDoc);

    coordinator->onStepUpBegin(operationContext(), 1LL);
    configurations = coordinator->getConfigurationsForTest();
    ASSERT_EQ(configurations.size(), 1U);
    assertContainsConfiguration(configurations, analyzerDoc);
}

TEST_F(QueryAnalysisCoordinatorTest, CreateActiveSamplersOnInsert) {
    auto coordinator = QueryAnalysisCoordinator::get(operationContext());

    // There are no samplers initially.
    auto samplers = coordinator->getSamplersForTest();
    ASSERT(samplers.empty());

    auto mongosDoc0 = makeConfigMongosDocument(mongosName0);
    uassertStatusOK(
        insertToConfigCollection(operationContext(), MongosType::ConfigNS, mongosDoc0.toBSON()));

    advanceTime(Seconds(1));
    auto mongosDoc1 = makeConfigMongosDocument(mongosName1);
    uassertStatusOK(
        insertToConfigCollection(operationContext(), MongosType::ConfigNS, mongosDoc1.toBSON()));

    // The inserts should cause two samplers to get created.
    samplers = coordinator->getSamplersForTest();
    ASSERT_EQ(samplers.size(), 2U);
    assertContainsSampler(samplers, mongosDoc0);
    assertContainsSampler(samplers, mongosDoc1);
}

TEST_F(QueryAnalysisCoordinatorTest, NotCreateInActiveSamplersOnInsert) {
    auto coordinator = QueryAnalysisCoordinator::get(operationContext());

    // There are no samplers initially.
    auto samplers = coordinator->getSamplersForTest();
    ASSERT(samplers.empty());

    auto mongosDoc = makeConfigMongosDocument(mongosName0);
    advanceTime(Seconds(inActiveThresholdSecs + 1));
    uassertStatusOK(
        insertToConfigCollection(operationContext(), MongosType::ConfigNS, mongosDoc.toBSON()));

    // The insert should not cause an inactive sampler to get created.
    samplers = coordinator->getSamplersForTest();
    ASSERT(samplers.empty());
}

TEST_F(QueryAnalysisCoordinatorTest, UpdateSamplersOnUpdate) {
    auto coordinator = QueryAnalysisCoordinator::get(operationContext());

    // There are no samplers initially.
    auto samplers = coordinator->getSamplersForTest();
    ASSERT(samplers.empty());

    auto mongosDocPreUpdate = makeConfigMongosDocument(mongosName0);
    uassertStatusOK(insertToConfigCollection(
        operationContext(), MongosType::ConfigNS, mongosDocPreUpdate.toBSON()));

    samplers = coordinator->getSamplersForTest();
    ASSERT_EQ(samplers.size(), 1U);
    assertContainsSampler(samplers, mongosDocPreUpdate);

    advanceTime(Seconds(1));
    auto mongosDocPostUpdate = makeConfigMongosDocument(mongosName0);
    ASSERT_GT(mongosDocPostUpdate.getPing(), mongosDocPreUpdate.getPing());
    uassertStatusOK(updateToConfigCollection(operationContext(),
                                             MongosType::ConfigNS,
                                             BSON(MongosType::name << mongosName0),
                                             mongosDocPostUpdate.toBSON(),
                                             false /* upsert */));

    // The update should cause the sampler to have the new ping time.
    samplers = coordinator->getSamplersForTest();
    ASSERT_EQ(samplers.size(), 1U);
    assertContainsSampler(samplers, mongosDocPostUpdate);
}

TEST_F(QueryAnalysisCoordinatorTest, RemoveSamplersOnDelete) {
    auto coordinator = QueryAnalysisCoordinator::get(operationContext());

    // There are no samplers initially.
    auto samplers = coordinator->getSamplersForTest();
    ASSERT(samplers.empty());

    auto mongosDoc = makeConfigMongosDocument(mongosName0);
    uassertStatusOK(
        insertToConfigCollection(operationContext(), MongosType::ConfigNS, mongosDoc.toBSON()));

    samplers = coordinator->getSamplersForTest();
    ASSERT_EQ(samplers.size(), 1U);
    assertContainsSampler(samplers, mongosDoc);

    uassertStatusOK(deleteToConfigCollection(
        operationContext(), MongosType::ConfigNS, mongosDoc.toBSON(), false /* multi */));

    // The delete should cause the sampler to get removed.
    samplers = coordinator->getSamplersForTest();
    ASSERT(samplers.empty());
}

TEST_F(QueryAnalysisCoordinatorTest, CreateSamplersOnStartup) {
    auto coordinator = QueryAnalysisCoordinator::get(operationContext());

    // There are no samplers initially.
    auto samplers = coordinator->getSamplersForTest();
    ASSERT(samplers.empty());

    // There are no samplers to create upon startup since there are no mongos documents.
    coordinator->onStartup(operationContext());
    samplers = coordinator->getSamplersForTest();
    ASSERT(samplers.empty());

    auto mongosDoc0 = makeConfigMongosDocument(mongosName0);
    uassertStatusOK(
        insertToConfigCollection(operationContext(), MongosType::ConfigNS, mongosDoc0.toBSON()));

    advanceTime(Seconds(inActiveThresholdSecs + 1));
    auto mongosDoc1 = makeConfigMongosDocument(mongosName1);
    uassertStatusOK(
        insertToConfigCollection(operationContext(), MongosType::ConfigNS, mongosDoc1.toBSON()));

    advanceTime(Seconds(1));
    auto mongosDoc2 = makeConfigMongosDocument(mongosName2);
    uassertStatusOK(
        insertToConfigCollection(operationContext(), MongosType::ConfigNS, mongosDoc2.toBSON()));

    coordinator->clearSamplersForTest();
    samplers = coordinator->getSamplersForTest();
    ASSERT(samplers.empty());

    // After startup, the samplers for mongos1 and mongos2 should get recreated. There should not be
    // a sampler for mongos0 since it is inactive.
    coordinator->onStartup(operationContext());
    samplers = coordinator->getSamplersForTest();
    ASSERT_EQ(samplers.size(), 2U);
    assertContainsSampler(samplers, mongosDoc1);
    assertContainsSampler(samplers, mongosDoc2);
}

TEST_F(QueryAnalysisCoordinatorTest, CreateSamplerOnGetNewConfigurations) {
    auto coordinator = QueryAnalysisCoordinator::get(operationContext());

    // There are no samplers initially.
    auto samplers = coordinator->getSamplersForTest();
    ASSERT(samplers.empty());

    auto pingTime = now();
    auto numQueriesExecutedPerSecond = 0.5;
    coordinator->getNewConfigurationsForSampler(
        operationContext(), mongosName0, numQueriesExecutedPerSecond);

    // The refresh should cause the sampler to get created.
    samplers = coordinator->getSamplersForTest();
    ASSERT_EQ(samplers.size(), 1U);
    assertContainsSampler(samplers, mongosName0, pingTime, numQueriesExecutedPerSecond);
}

TEST_F(QueryAnalysisCoordinatorTest, UpdateSamplerOnGetNewConfigurations) {
    auto coordinator = QueryAnalysisCoordinator::get(operationContext());

    // There are no samplers initially.
    auto samplers = coordinator->getSamplersForTest();
    ASSERT(samplers.empty());

    auto oldPingTime = now();
    auto oldNumQueriesExecutedPerSecond = 0.5;
    coordinator->getNewConfigurationsForSampler(
        operationContext(), mongosName0, oldNumQueriesExecutedPerSecond);

    // The refresh should cause the sampler to get created.
    samplers = coordinator->getSamplersForTest();
    ASSERT_EQ(samplers.size(), 1U);
    assertContainsSampler(samplers, mongosName0, oldPingTime, oldNumQueriesExecutedPerSecond);

    advanceTime(Seconds(1));

    auto newPingTime = now();
    auto newNumQueriesExecutedPerSecond = 1;
    ASSERT_GT(newPingTime, oldPingTime);
    coordinator->getNewConfigurationsForSampler(
        operationContext(), mongosName0, newNumQueriesExecutedPerSecond);

    // The refresh should cause the sampler to get updated.
    samplers = coordinator->getSamplersForTest();
    ASSERT_EQ(samplers.size(), 1U);
    assertContainsSampler(samplers, mongosName0, newPingTime, newNumQueriesExecutedPerSecond);
}

TEST_F(QueryAnalysisCoordinatorTest, ResetLastNumQueriesExecutedOnStepUp) {
    auto coordinator = QueryAnalysisCoordinator::get(operationContext());

    // There are no samplers initially.
    auto samplers = coordinator->getSamplersForTest();
    ASSERT(samplers.empty());

    auto pingTime = now();
    auto numQueriesExecutedPerSecond = 0.5;
    coordinator->getNewConfigurationsForSampler(
        operationContext(), mongosName0, numQueriesExecutedPerSecond);

    // The refresh should cause the sampler to get created.
    samplers = coordinator->getSamplersForTest();
    ASSERT_EQ(samplers.size(), 1U);
    assertContainsSampler(samplers, mongosName0, pingTime, numQueriesExecutedPerSecond);

    coordinator->onStepUpBegin(operationContext(), 1LL);

    // After stepup, the sampler should have an unknown number of queries executed per second.
    samplers = coordinator->getSamplersForTest();
    assertContainsSampler(
        samplers, mongosName0, pingTime, boost::none /* numQueriesExecutedPerSecond */);
}

TEST_F(QueryAnalysisCoordinatorTest, GetNewConfigurationsOneSamplerBasic) {
    auto coordinator = QueryAnalysisCoordinator::get(operationContext());

    auto startTime0 = now();
    auto analyzerDoc0 = makeConfigQueryAnalyzersDocument(
        nss0, collUuid0, QueryAnalyzerModeEnum::kFull, 0.5, startTime0);
    uassertStatusOK(insertToConfigCollection(operationContext(),
                                             NamespaceString::kConfigQueryAnalyzersNamespace,
                                             analyzerDoc0.toBSON()));

    advanceTime(Seconds(1));

    auto startTime1 = now();
    auto analyzerDoc1 = makeConfigQueryAnalyzersDocument(
        nss1, collUuid1, QueryAnalyzerModeEnum::kFull, 5, startTime1);
    uassertStatusOK(insertToConfigCollection(operationContext(),
                                             NamespaceString::kConfigQueryAnalyzersNamespace,
                                             analyzerDoc1.toBSON()));

    auto configurations =
        coordinator->getNewConfigurationsForSampler(operationContext(), mongosName0, 1);
    ASSERT_EQ(configurations.size(), 2U);
    assertContainsConfiguration(configurations,
                                analyzerDoc0.getNs(),
                                analyzerDoc0.getCollectionUuid(),
                                *analyzerDoc0.getSampleRate(),
                                startTime0);
    assertContainsConfiguration(configurations,
                                analyzerDoc1.getNs(),
                                analyzerDoc1.getCollectionUuid(),
                                *analyzerDoc1.getSampleRate(),
                                startTime1);
}

TEST_F(QueryAnalysisCoordinatorTest, GetNewConfigurationsOneSamplerOneDisabledColl) {
    auto coordinator = QueryAnalysisCoordinator::get(operationContext());

    auto startTime0 = now();
    auto analyzerDoc0 = makeConfigQueryAnalyzersDocument(
        nss0, collUuid0, QueryAnalyzerModeEnum::kOff, boost::none /* sampleRate */, startTime0);
    uassertStatusOK(insertToConfigCollection(operationContext(),
                                             NamespaceString::kConfigQueryAnalyzersNamespace,
                                             analyzerDoc0.toBSON()));

    advanceTime(Seconds(1));

    auto startTime1 = now();
    auto analyzerDoc1 = makeConfigQueryAnalyzersDocument(
        nss1, collUuid1, QueryAnalyzerModeEnum::kFull, 5, startTime1);
    uassertStatusOK(insertToConfigCollection(operationContext(),
                                             NamespaceString::kConfigQueryAnalyzersNamespace,
                                             analyzerDoc1.toBSON()));


    auto configurations =
        coordinator->getNewConfigurationsForSampler(operationContext(), mongosName0, 1);
    ASSERT_EQ(configurations.size(), 1U);
    assertContainsConfiguration(configurations,
                                analyzerDoc1.getNs(),
                                analyzerDoc1.getCollectionUuid(),
                                *analyzerDoc1.getSampleRate(),
                                startTime1);
}

TEST_F(QueryAnalysisCoordinatorTest, GetNewConfigurationsMultipleSamplersBasic) {
    auto coordinator = QueryAnalysisCoordinator::get(operationContext());

    // There are no samplers initially.
    auto samplers = coordinator->getSamplersForTest();
    ASSERT(samplers.empty());

    auto mongosDoc0 = makeConfigMongosDocument(mongosName0);
    uassertStatusOK(
        insertToConfigCollection(operationContext(), MongosType::ConfigNS, mongosDoc0.toBSON()));
    advanceTime(Seconds(1));
    auto mongosDoc1 = makeConfigMongosDocument(mongosName1);
    uassertStatusOK(
        insertToConfigCollection(operationContext(), MongosType::ConfigNS, mongosDoc1.toBSON()));

    // The inserts should cause two samplers to get created.
    samplers = coordinator->getSamplersForTest();
    ASSERT_EQ(samplers.size(), 2U);

    auto startTime0 = now();
    auto analyzerDoc0 = makeConfigQueryAnalyzersDocument(
        nss0, collUuid0, QueryAnalyzerModeEnum::kFull, 1, startTime0);
    uassertStatusOK(insertToConfigCollection(operationContext(),
                                             NamespaceString::kConfigQueryAnalyzersNamespace,
                                             analyzerDoc0.toBSON()));

    advanceTime(Seconds(1));

    auto startTime1 = now();
    auto analyzerDoc1 = makeConfigQueryAnalyzersDocument(
        nss1, collUuid1, QueryAnalyzerModeEnum::kFull, 15.5, startTime1);
    uassertStatusOK(insertToConfigCollection(operationContext(),
                                             NamespaceString::kConfigQueryAnalyzersNamespace,
                                             analyzerDoc1.toBSON()));

    // Query distribution after: [1, unknown].
    auto configurations0 =
        coordinator->getNewConfigurationsForSampler(operationContext(), mongosName0, 1);
    double expectedRatio0 = 0.5;
    assertContainsConfiguration(configurations0,
                                analyzerDoc0.getNs(),
                                analyzerDoc0.getCollectionUuid(),
                                expectedRatio0 * analyzerDoc0.getSampleRate().get(),
                                startTime0);
    assertContainsConfiguration(configurations0,
                                analyzerDoc1.getNs(),
                                analyzerDoc1.getCollectionUuid(),
                                expectedRatio0 * analyzerDoc1.getSampleRate().get(),
                                startTime1);

    // Query distribution after: [1, 4.5].
    auto configurations1 =
        coordinator->getNewConfigurationsForSampler(operationContext(), mongosName1, 4.5);
    double expectedRatio1 = 4.5 / 5.5;
    ASSERT_EQ(configurations1.size(), 2U);
    assertContainsConfiguration(configurations1,
                                analyzerDoc0.getNs(),
                                analyzerDoc0.getCollectionUuid(),
                                expectedRatio1 * analyzerDoc0.getSampleRate().get(),
                                startTime0);
    assertContainsConfiguration(configurations1,
                                analyzerDoc1.getNs(),
                                analyzerDoc1.getCollectionUuid(),
                                expectedRatio1 * analyzerDoc1.getSampleRate().get(),
                                startTime1);

    // Query distribution after: [1.5, 4.5].
    configurations0 =
        coordinator->getNewConfigurationsForSampler(operationContext(), mongosName0, 1.5);
    expectedRatio0 = 1.5 / 6;
    assertContainsConfiguration(configurations0,
                                analyzerDoc0.getNs(),
                                analyzerDoc0.getCollectionUuid(),
                                expectedRatio0 * analyzerDoc0.getSampleRate().get(),
                                startTime0);
    assertContainsConfiguration(configurations0,
                                analyzerDoc1.getNs(),
                                analyzerDoc1.getCollectionUuid(),
                                expectedRatio0 * analyzerDoc1.getSampleRate().get(),
                                startTime1);

    // Query distribution after: [1.5, 0].
    configurations1 =
        coordinator->getNewConfigurationsForSampler(operationContext(), mongosName1, 0);
    assertContainsConfiguration(
        configurations1, analyzerDoc0.getNs(), analyzerDoc0.getCollectionUuid(), 0.0, startTime0);

    // Query distribution after: [0, 0].
    configurations0 =
        coordinator->getNewConfigurationsForSampler(operationContext(), mongosName0, 0);
    expectedRatio0 = 0.5;
    assertContainsConfiguration(configurations0,
                                analyzerDoc0.getNs(),
                                analyzerDoc0.getCollectionUuid(),
                                expectedRatio0 * analyzerDoc0.getSampleRate().get(),
                                startTime0);
    assertContainsConfiguration(configurations0,
                                analyzerDoc1.getNs(),
                                analyzerDoc1.getCollectionUuid(),
                                expectedRatio0 * analyzerDoc1.getSampleRate().get(),
                                startTime1);
}

}  // namespace
}  // namespace analyze_shard_key
}  // namespace mongo
