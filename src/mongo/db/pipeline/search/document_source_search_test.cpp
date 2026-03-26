/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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
#include "mongo/db/pipeline/search/document_source_search.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/search/document_source_internal_search_id_lookup.h"
#include "mongo/db/pipeline/search/document_source_internal_search_mongot_remote.h"
#include "mongo/db/pipeline/search/search_helper_bson_obj.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/query/search/mongot_options.h"
#include "mongo/db/shard_role/shard_catalog/operation_sharding_state.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"

#include <boost/intrusive_ptr.hpp>

namespace mongo {

namespace {

using boost::intrusive_ptr;
using std::list;
using std::vector;

class SearchTest : service_context_test::WithSetupTransportLayer,
                   public AggregationContextFixture {};

struct MockMongoInterface final : public StubMongoProcessInterface {
    bool inShardedEnvironment(OperationContext* opCtx) const override {
        return false;
    }
};

TEST_F(SearchTest, ShouldSerializeAllNecessaryFieldsAtUnspecifiedVerbosity) {
    const auto mongotQuery = fromjson("{term: 'asdf'}");
    const auto stageObj = BSON("$search" << mongotQuery);

    auto expCtx = getExpCtx();
    expCtx->setMongoProcessInterface(std::make_unique<MockMongoInterface>());
    expCtx->setUUID(UUID::gen());

    intrusive_ptr<DocumentSource> searchDS =
        DocumentSourceSearch::createFromBson(stageObj.firstElement(), expCtx);
    list<intrusive_ptr<DocumentSource>> results =
        dynamic_cast<DocumentSourceSearch*>(searchDS.get())->desugar();
    ASSERT_EQUALS(results.size(), 2UL);

    const auto* mongotRemoteStage =
        dynamic_cast<DocumentSourceInternalSearchMongotRemote*>(results.front().get());
    ASSERT(mongotRemoteStage);

    const auto* idLookupStage =
        dynamic_cast<DocumentSourceInternalSearchIdLookUp*>(results.back().get());
    ASSERT(idLookupStage);

    vector<Value> explainedStages;
    mongotRemoteStage->serializeToArray(explainedStages);
    idLookupStage->serializeToArray(explainedStages);
    ASSERT_EQUALS(explainedStages.size(), 2UL);

    auto mongotRemoteExplain = explainedStages[0];
    ASSERT_DOCUMENT_EQ(mongotRemoteExplain.getDocument(),
                       Document({{"$_internalSearchMongotRemote", Document(mongotQuery)}}));

    auto idLookupExplain = explainedStages[1];
    ASSERT_DOCUMENT_EQ(idLookupExplain.getDocument(),
                       Document({{"$_internalSearchIdLookup", Document()}}));
}

TEST_F(SearchTest, ShouldFailToParseIfSpecIsNotObject) {
    const auto specObj = fromjson("{$search: 1}");
    ASSERT_THROWS_CODE(DocumentSourceSearch::createFromBson(specObj.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::FailedToParse);
}

using SearchDeathTest = SearchTest;

DEATH_TEST_F(SearchDeathTest,
             TassertsWhenRouterSendsExtensionFlagButExtensionNotLoaded,
             "12230700") {
    auto opCtx = getExpCtx()->getOperationContext();

    // Set shard role to simulate request coming from mongos.
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.coll");
    ScopedSetShardRole scopedSetShardRole{
        opCtx, nss, ShardVersion::UNTRACKED(), boost::none /* databaseVersion */};

    // Simulate router sending featureFlagSearchExtension=true.
    auto& flag = feature_flags::gFeatureFlagSearchExtension;
    std::vector<BSONObj> flagValues{BSON("name" << flag.getName() << "value" << true)};
    auto ifrContext = std::make_shared<IncrementalFeatureRolloutContext>(flagValues);

    auto spec = fromjson(R"({
        $search: {
            term: "asdf"
        }
    })");

    // Parse the $search stage. Since the extension is not loaded (only the fallback parser
    // is registered), this should trigger the tassert when the router sent the flag as true.
    LiteParsedDocumentSource::parse(
        nss, spec, LiteParserOptions{.ifrContext = ifrContext, .opCtx = opCtx});
}

TEST_F(SearchTest, UsesFallbackLegacyParserWhenSearchExtensionFlagIsFalse) {
    auto opCtx = getExpCtx()->getOperationContext();

    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.coll");

    // Simulate router sending featureFlagSearchExtension=false.
    auto& flag = feature_flags::gFeatureFlagSearchExtension;
    std::vector<BSONObj> flagValues{BSON("name" << flag.getName() << "value" << false)};
    auto ifrContext = std::make_shared<IncrementalFeatureRolloutContext>(flagValues);

    auto spec = fromjson(R"({
        $search: {
            term: "asdf"
        }
    })");

    // Should successfully parse using the fallback legacy implementation.
    ASSERT_DOES_NOT_THROW(LiteParsedDocumentSource::parse(
        nss, spec, LiteParserOptions{.ifrContext = ifrContext, .opCtx = opCtx}));
}

TEST_F(SearchTest, IsExtensionMongotPipelineReturnsTrueForSearch) {
    auto& flag = feature_flags::gFeatureFlagSearchExtension;
    std::vector<BSONObj> flagValues{BSON("name" << flag.getName() << "value" << true)};
    auto ifrContext = std::make_shared<IncrementalFeatureRolloutContext>(flagValues);

    auto origExtensions = serverGlobalParams.extensions;
    ScopeGuard restoreExtensions([&] { serverGlobalParams.extensions = origExtensions; });
    serverGlobalParams.extensions.push_back("mongot-extension");

    auto pipeline = std::vector<BSONObj>{
        fromjson(R"({$search: {index: "idx", text: {query: "a", path: "b"}}})")};

    ASSERT_TRUE(search_helper_bson_obj::isExtensionMongotPipeline(ifrContext, pipeline));
    ASSERT_FALSE(search_helper_bson_obj::isMongotPipeline(ifrContext, pipeline));
}

TEST_F(SearchTest, IsExtensionMongotPipelineReturnsFalseForSearchFlagDisabled) {
    // No flag values, so featureFlagSearchExtension is false.
    auto ifrContext = std::make_shared<IncrementalFeatureRolloutContext>(std::vector<BSONObj>{});

    auto origExtensions = serverGlobalParams.extensions;
    ScopeGuard restoreExtensions([&] { serverGlobalParams.extensions = origExtensions; });
    serverGlobalParams.extensions.push_back("mongot-extension");

    auto pipeline = std::vector<BSONObj>{
        fromjson(R"({$search: {index: "idx", text: {query: "a", path: "b"}}})")};

    ASSERT_FALSE(search_helper_bson_obj::isExtensionMongotPipeline(ifrContext, pipeline));
    ASSERT_TRUE(search_helper_bson_obj::isMongotPipeline(ifrContext, pipeline));
}

TEST_F(SearchTest, IsExtensionMongotPipelineReturnsTrueForSearchMeta) {
    auto& flag = feature_flags::gFeatureFlagSearchExtension;
    std::vector<BSONObj> flagValues{BSON("name" << flag.getName() << "value" << true)};
    auto ifrContext = std::make_shared<IncrementalFeatureRolloutContext>(flagValues);

    auto origExtensions = serverGlobalParams.extensions;
    ScopeGuard restoreExtensions([&] { serverGlobalParams.extensions = origExtensions; });
    serverGlobalParams.extensions.push_back("mongot-extension");

    auto pipeline = std::vector<BSONObj>{
        fromjson(R"({$searchMeta: {index: "idx", text: {query: "a", path: "b"}}})")};

    ASSERT_TRUE(search_helper_bson_obj::isExtensionMongotPipeline(ifrContext, pipeline));
    ASSERT_FALSE(search_helper_bson_obj::isMongotPipeline(ifrContext, pipeline));
}

TEST_F(SearchTest, IsExtensionMongotPipelineReturnsFalseForSearchMetaFlagDisabled) {
    // No flag values, so featureFlagSearchExtension is false.
    auto ifrContext = std::make_shared<IncrementalFeatureRolloutContext>(std::vector<BSONObj>{});

    auto origExtensions = serverGlobalParams.extensions;
    ScopeGuard restoreExtensions([&] { serverGlobalParams.extensions = origExtensions; });
    serverGlobalParams.extensions.push_back("mongot-extension");

    auto pipeline = std::vector<BSONObj>{
        fromjson(R"({$searchMeta: {index: "idx", text: {query: "a", path: "b"}}})")};

    ASSERT_FALSE(search_helper_bson_obj::isExtensionMongotPipeline(ifrContext, pipeline));
    ASSERT_TRUE(search_helper_bson_obj::isMongotPipeline(ifrContext, pipeline));
}

}  // namespace
}  // namespace mongo
