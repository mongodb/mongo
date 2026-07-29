// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/db/pipeline/search/document_source_search.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/document_metadata_fields.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_add_fields.h"
#include "mongo/db/pipeline/document_source_internal_document_results_and_metadata.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/search/document_source_internal_search_id_lookup.h"
#include "mongo/db/pipeline/search/document_source_internal_search_mongot_remote.h"
#include "mongo/db/pipeline/search/search_helper.h"
#include "mongo/db/pipeline/search/search_helper_bson_obj.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/query/search/mongot_options.h"
#include "mongo/db/shard_role/shard_catalog/operation_sharding_state.h"
#include "mongo/transport/mock_session.h"
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

// One spec per internal-only field. Used by both the external-client rejection test and the
// internal-client acceptance test so the two stay in lock-step as fields are added.
static const std::vector<std::pair<std::string, std::string>> kInternalSearchFieldCases = {
    {"mongotQuery",
     R"({$search: {mongotQuery: {index: "default", text: {query: "hello", path: "body"}}}})"},
    {"mergingPipeline",
     R"({$search: {mergingPipeline: [{$lookup: {from: "secret", as: "leak", pipeline: []}}]}})"},
    {"metadataMergeProtocolVersion", R"({$search: {metadataMergeProtocolVersion: 1}})"},
    {"requiresSearchSequenceToken", R"({$search: {requiresSearchSequenceToken: true}})"},
    {"requiresSearchMetaCursor", R"({$search: {requiresSearchMetaCursor: true}})"},
    {"view",
     R"({$search: {view: {name: "secretView", effectivePipeline: [{$match: {leaked: true}}]}}})"},
    {"limit", R"({$search: {limit: 100}})"},
    {"sortSpec", R"({$search: {sortSpec: {field: 1}}})"},
    {"mongotDocsRequested", R"({$search: {mongotDocsRequested: 50}})"},
    {"docsNeededBounds", R"({$search: {docsNeededBounds: {minBounds: 1, maxBounds: 100}}})"},
};

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

TEST_F(SearchTest, GetSortPatternReturnsDefaultSearchScoreWhenNoSortSpec) {
    const auto stageObj = BSON("$search" << fromjson("{term: 'asdf'}"));

    intrusive_ptr<DocumentSource> searchDS =
        DocumentSourceSearch::createFromBson(stageObj.firstElement(), getExpCtx());
    auto sortPattern = searchDS->getSortPattern();
    ASSERT_EQ(sortPattern.size(), 1u);
    ASSERT_FALSE(sortPattern[0].isAscending);
    ASSERT_TRUE(sortPattern[0].expression);
    ASSERT_EQ(sortPattern[0].expression->getMetaType(), DocumentMetadataFields::kSearchScore);
}

TEST_F(SearchTest, GetSortPatternHonorsMongotSortSpec) {
    // Pass a non-owned BSONObj view into the spec to exercise the getOwned() codepath.
    auto sortSpecBacking = BSON("customField" << 1);
    InternalSearchMongotRemoteSpec spec(fromjson("{term: 'asdf'}").getOwned());
    spec.setSortSpec(BSONObj(sortSpecBacking.objdata()));

    auto searchDS = make_intrusive<DocumentSourceSearch>(getExpCtx(), std::move(spec));
    auto sortPattern = searchDS->getSortPattern();
    ASSERT_EQ(sortPattern.size(), 1u);
    ASSERT_TRUE(sortPattern[0].isAscending);
    ASSERT_TRUE(sortPattern[0].fieldPath);
    ASSERT_EQ(sortPattern[0].fieldPath->fullPath(), "customField");
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
    auto ifrContext = IncrementalFeatureRolloutContext::forTest(flagValues);

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
    auto ifrContext = IncrementalFeatureRolloutContext::forTest(flagValues);

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
    auto ifrContext = IncrementalFeatureRolloutContext::forTest(flagValues);

    auto origExtensions = serverGlobalParams.extensions;
    ScopeGuard restoreExtensions([&] { serverGlobalParams.extensions = origExtensions; });
    serverGlobalParams.extensions.push_back(
        std::string{search_helper_bson_obj::detail::kMongotExtensionName});

    auto pipeline = std::vector<BSONObj>{
        fromjson(R"({$search: {index: "idx", text: {query: "a", path: "b"}}})")};

    ASSERT_TRUE(search_helper_bson_obj::isExtensionMongotPipeline(ifrContext, pipeline));
    ASSERT_FALSE(search_helper_bson_obj::isMongotPipeline(ifrContext, pipeline));
}

TEST_F(SearchTest, IsExtensionMongotPipelineReturnsFalseForSearchFlagDisabled) {
    // Disable extensions for this test.
    auto& flag = feature_flags::gFeatureFlagSearchExtension;
    auto ifrContext = IncrementalFeatureRolloutContext::forTest(
        std::vector<BSONObj>{BSON("name" << flag.getName() << "value" << false)});

    auto origExtensions = serverGlobalParams.extensions;
    ScopeGuard restoreExtensions([&] { serverGlobalParams.extensions = origExtensions; });
    serverGlobalParams.extensions.push_back(
        std::string{search_helper_bson_obj::detail::kMongotExtensionName});

    auto pipeline = std::vector<BSONObj>{
        fromjson(R"({$search: {index: "idx", text: {query: "a", path: "b"}}})")};

    ASSERT_FALSE(search_helper_bson_obj::isExtensionMongotPipeline(ifrContext, pipeline));
    ASSERT_TRUE(search_helper_bson_obj::isMongotPipeline(ifrContext, pipeline));
}

TEST_F(SearchTest, IsExtensionMongotPipelineReturnsTrueForSearchMeta) {
    auto& flag = feature_flags::gFeatureFlagSearchExtension;
    std::vector<BSONObj> flagValues{BSON("name" << flag.getName() << "value" << true)};
    auto ifrContext = IncrementalFeatureRolloutContext::forTest(flagValues);

    auto origExtensions = serverGlobalParams.extensions;
    ScopeGuard restoreExtensions([&] { serverGlobalParams.extensions = origExtensions; });
    serverGlobalParams.extensions.push_back(
        std::string{search_helper_bson_obj::detail::kMongotExtensionName});

    auto pipeline = std::vector<BSONObj>{
        fromjson(R"({$searchMeta: {index: "idx", text: {query: "a", path: "b"}}})")};

    ASSERT_TRUE(search_helper_bson_obj::isExtensionMongotPipeline(ifrContext, pipeline));
    ASSERT_FALSE(search_helper_bson_obj::isMongotPipeline(ifrContext, pipeline));
}

TEST_F(SearchTest, IsExtensionMongotPipelineReturnsFalseForSearchMetaFlagDisabled) {
    auto& flag = feature_flags::gFeatureFlagSearchExtension;
    std::vector<BSONObj> flagValues{BSON("name" << flag.getName() << "value" << false)};
    auto ifrContext = IncrementalFeatureRolloutContext::forTest(flagValues);

    auto origExtensions = serverGlobalParams.extensions;
    ScopeGuard restoreExtensions([&] { serverGlobalParams.extensions = origExtensions; });
    serverGlobalParams.extensions.push_back(
        std::string{search_helper_bson_obj::detail::kMongotExtensionName});

    auto pipeline = std::vector<BSONObj>{
        fromjson(R"({$searchMeta: {index: "idx", text: {query: "a", path: "b"}}})")};

    ASSERT_FALSE(search_helper_bson_obj::isExtensionMongotPipeline(ifrContext, pipeline));
    ASSERT_TRUE(search_helper_bson_obj::isMongotPipeline(ifrContext, pipeline));
}

TEST_F(SearchTest, AssertSearchMetaAccessValidForDocumentResultsAndMetadataWrappingSearch) {
    auto expCtx = getExpCtx();
    auto searchStage = DocumentSourceSearch::createFromBson(
        BSON("$search" << fromjson("{term: 'asdf'}")).firstElement(), expCtx);
    DocumentSourceContainer pipeline{
        DocumentSourceInternalDocumentResultsAndMetadata::create(
            expCtx, searchStage, MetadataBindSpec("SEARCH_META")),
        DocumentSourceAddFields::createFromBson(
            BSON("$addFields" << BSON("meta" << "$$SEARCH_META")).firstElement(), expCtx)};
    ASSERT_DOES_NOT_THROW(search_helpers::assertSearchMetaAccessValid(pipeline, expCtx.get()));
}

TEST_F(SearchTest,
       AssertSearchMetaAccessValidThrowsForDocumentResultsAndMetadataWrappingNonExtension) {
    // $_internalDocumentResultsAndMetadata wrapping a non-extension stage must not be credited
    // as setting $$SEARCH_META, so accessing $$SEARCH_META afterwards must fail.
    auto expCtx = getExpCtx();
    auto nonExtensionStage = DocumentSourceAddFields::createFromBson(
        BSON("$addFields" << BSON("x" << 1)).firstElement(), expCtx);
    DocumentSourceContainer pipeline{
        DocumentSourceInternalDocumentResultsAndMetadata::create(
            expCtx, nonExtensionStage, boost::none),
        DocumentSourceAddFields::createFromBson(
            BSON("$addFields" << BSON("meta" << "$$SEARCH_META")).firstElement(), expCtx)};
    ASSERT_THROWS_CODE(search_helpers::assertSearchMetaAccessValid(pipeline, expCtx.get()),
                       AssertionException,
                       6347902);
}

TEST_F(SearchTest, AssertSearchMetaAccessValidThrowsWhenNoSetterPresent) {
    auto expCtx = getExpCtx();
    DocumentSourceContainer pipeline{DocumentSourceAddFields::createFromBson(
        BSON("$addFields" << BSON("meta" << "$$SEARCH_META")).firstElement(), expCtx)};
    ASSERT_THROWS_CODE(search_helpers::assertSearchMetaAccessValid(pipeline, expCtx.get()),
                       AssertionException,
                       6347902);
}

TEST_F(SearchTest,
       IsExtensionMongotPipelineReturnsTrueForDocumentResultsAndMetadataWrappingSearch) {
    auto expCtx = getExpCtx();
    auto searchStage = DocumentSourceSearch::createFromBson(
        BSON("$search" << fromjson("{term: 'asdf'}")).firstElement(), expCtx);
    DocumentSourceContainer stages{
        DocumentSourceInternalDocumentResultsAndMetadata::create(expCtx, searchStage, boost::none)};
    auto pipeline = Pipeline::create(std::move(stages), expCtx);
    ASSERT_TRUE(search_helpers::isExtensionMongotPipeline(pipeline.get()));
}

TEST_F(SearchTest,
       IsExtensionMongotPipelineReturnsFalseForDocumentResultsAndMetadataWrappingNonExtension) {
    // $_internalDocumentResultsAndMetadata wrapping a generic stage (e.g. $addFields) must not
    // be misclassified as an extension mongot pipeline.
    auto expCtx = getExpCtx();
    auto nonExtensionStage = DocumentSourceAddFields::createFromBson(
        BSON("$addFields" << BSON("x" << 1)).firstElement(), expCtx);
    DocumentSourceContainer stages{DocumentSourceInternalDocumentResultsAndMetadata::create(
        expCtx, nonExtensionStage, boost::none)};
    auto pipeline = Pipeline::create(std::move(stages), expCtx);
    ASSERT_FALSE(search_helpers::isExtensionMongotPipeline(pipeline.get()));
}

// Each internal routing field must be individually rejected when supplied by an external client.
TEST_F(SearchTest, ExternalClientCannotSupplyInternalSearchFields) {
    auto session = transport::MockSession::create(nullptr);
    auto externalClient = getServiceContext()->getService()->makeClient("externalClient", session);
    auto externalOpCtx = externalClient->makeOperationContext();

    auto nss = getExpCtx()->getNamespaceString();
    for (const auto& [fieldName, specJson] : kInternalSearchFieldCases) {
        SCOPED_TRACE(fieldName);
        const auto specBson = fromjson(specJson);
        auto lpds = SearchLiteParsed::parse(nss, specBson.firstElement(), LiteParserOptions{});
        ASSERT_THROWS_CODE(lpds->validate(externalOpCtx.get()), AssertionException, 5491300);
    }
}

// Internal clients (no transport session) must still be able to supply internal routing fields,
// since they are set by the router during sharded search planning. Iterates the same case list
// as the external-client test so the two stay in sync.
TEST_F(SearchTest, InternalClientCanSupplyInternalSearchFields) {
    // The default test client has no transport session and is treated as internal.
    auto opCtx = getExpCtx()->getOperationContext();
    auto nss = getExpCtx()->getNamespaceString();

    for (const auto& [fieldName, specJson] : kInternalSearchFieldCases) {
        SCOPED_TRACE(fieldName);
        const auto specBson = fromjson(specJson);
        auto lpds = SearchLiteParsed::parse(nss, specBson.firstElement(), LiteParserOptions{});
        ASSERT_DOES_NOT_THROW(lpds->validate(opCtx));
    }
}

// createFromBson must accept a spec already in its serialized internal ('mongotQuery') form;
// validation of those fields belongs to the LiteParse layer.
TEST_F(SearchTest, CreateFromBsonAcceptsSerializedInternalSpec) {
    auto expCtx = getExpCtx();
    expCtx->setMongoProcessInterface(std::make_unique<MockMongoInterface>());

    const auto serializedSpec = fromjson(R"({
        $search: {
            mongotQuery: {index: "default", text: {query: "hello", path: "body"}, count: {type: "total"}},
            metadataMergeProtocolVersion: 1,
            mergingPipeline: []
        }
    })");

    ASSERT_DOES_NOT_THROW(
        DocumentSourceSearch::createFromBson(serializedSpec.firstElement(), expCtx));
}

}  // namespace
}  // namespace mongo
