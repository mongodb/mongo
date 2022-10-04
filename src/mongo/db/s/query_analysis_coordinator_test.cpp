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

    QueryAnalyzerDocument makeConfigQueryAnalyzersDocument(
        const NamespaceString& nss,
        const UUID& collUuid,
        QueryAnalyzerModeEnum mode,
        boost::optional<double> sampleRate = boost::none) {
        QueryAnalyzerDocument doc;
        doc.setNs(nss);
        doc.setCollectionUuid(collUuid);
        QueryAnalyzerConfiguration configuration;
        configuration.setMode(mode);
        configuration.setSampleRate(sampleRate);
        doc.setConfiguration(configuration);
        return doc;
    }

    void assertContainsConfiguration(
        const std::map<UUID, CollectionQueryAnalyzerConfiguration>& configurations,
        QueryAnalyzerDocument analyzerDoc) {
        auto it = configurations.find(analyzerDoc.getCollectionUuid());
        ASSERT(it != configurations.end());
        auto& configuration = it->second;
        ASSERT_EQ(configuration.getNs(), analyzerDoc.getNs());
        ASSERT_EQ(configuration.getSampleRate(), *analyzerDoc.getSampleRate());
    }

protected:
    const NamespaceString nss0{"testDb", "testColl0"};
    const NamespaceString nss1{"testDb", "testColl1"};
    const NamespaceString nss2{"testDb", "testColl2"};

    const UUID collUuid0 = UUID::gen();
    const UUID collUuid1 = UUID::gen();
    const UUID collUuid2 = UUID::gen();

private:
    RAIIServerParameterControllerForTest _featureFlagController{"featureFlagAnalyzeShardKey", true};
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

}  // namespace
}  // namespace analyze_shard_key
}  // namespace mongo
