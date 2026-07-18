// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/search/document_source_search_meta.h"

#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_replace_root.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/document_source_union_with.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/search/document_source_internal_search_mongot_remote.h"
#include "mongo/db/query/search/mongot_options.h"
#include "mongo/db/shard_role/shard_catalog/operation_sharding_state.h"
#include "mongo/transport/mock_session.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <vector>

#include <boost/intrusive_ptr.hpp>

namespace mongo {

namespace {

using boost::intrusive_ptr;
using std::list;
using std::vector;

// One spec per internal-only field. Used by both the external-client rejection test and the
// internal-client acceptance test so the two stay in lock-step as fields are added.
static const std::vector<std::pair<std::string, std::string>> kInternalSearchMetaFieldCases = {
    {"mongotQuery",
     R"({$searchMeta: {mongotQuery: {index: "default", text: {query: "hello", path: "body"}}}})"},
    {"mergingPipeline", R"({$searchMeta: {mergingPipeline: [{$merge: {into: "secret"}}]}})"},
    {"metadataMergeProtocolVersion", R"({$searchMeta: {metadataMergeProtocolVersion: 1}})"},
    {"requiresSearchSequenceToken", R"({$searchMeta: {requiresSearchSequenceToken: true}})"},
    {"requiresSearchMetaCursor", R"({$searchMeta: {requiresSearchMetaCursor: true}})"},
    {"view",
     R"({$searchMeta: {view: {name: "secretView", effectivePipeline: [{$match: {leaked: true}}]}}})"},
    {"limit", R"({$searchMeta: {limit: 100}})"},
    {"sortSpec", R"({$searchMeta: {sortSpec: {field: 1}}})"},
    {"mongotDocsRequested", R"({$searchMeta: {mongotDocsRequested: 50}})"},
    {"docsNeededBounds", R"({$searchMeta: {docsNeededBounds: {minBounds: 1, maxBounds: 100}}})"},
};

class SearchMetaTest : service_context_test::WithSetupTransportLayer,
                       public AggregationContextFixture {};

struct MockMongoInterface final : public StubMongoProcessInterface {
    bool inShardedEnvironment(OperationContext* opCtx) const override {
        return false;
    }
};

TEST_F(SearchMetaTest, TestParsingOfSearchMeta) {
    const auto mongotQuery = fromjson("{query: 'cakes', path: 'title'}");
    auto specObj = BSON("$searchMeta" << mongotQuery);

    auto expCtx = getExpCtx();
    expCtx->setMongoProcessInterface(std::make_unique<MockMongoInterface>());
    auto fromNs = NamespaceString::createNamespaceString_forTest("unittests.$cmd.aggregate");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});
    list<intrusive_ptr<DocumentSource>> results =
        DocumentSourceSearchMeta::createFromBson(specObj.firstElement(), expCtx);

    ASSERT_EQUALS(results.size(), 1UL);
    ASSERT(dynamic_cast<DocumentSourceSearchMeta*>(results.begin()->get()));

    // $searchMeta argument must be an object.
    specObj = BSON("$searchMeta" << 1000);
    ASSERT_THROWS_CODE(
        DocumentSourceSearchMeta::createFromBson(specObj.firstElement(), getExpCtx()),
        AssertionException,
        ErrorCodes::FailedToParse);
}

using SearchMetaDeathTest = SearchMetaTest;

DEATH_TEST_F(SearchMetaDeathTest,
             TassertsWhenRouterSendsExtensionFlagButExtensionNotLoaded,
             "12230701") {
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
        $searchMeta: {
            query: "cakes",
            path: "title"
        }
    })");

    // Parse the $searchMeta stage. Since the extension is not loaded (only the fallback parser
    // is registered), this should trigger the tassert when the router sent the flag as true.
    LiteParsedDocumentSource::parse(
        nss, spec, LiteParserOptions{.ifrContext = ifrContext, .opCtx = opCtx});
}

TEST_F(SearchMetaTest, UsesFallbackLegacyParserWhenSearchExtensionFlagIsFalse) {
    auto opCtx = getExpCtx()->getOperationContext();

    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.coll");

    // Simulate router sending featureFlagSearchExtension=false.
    auto& flag = feature_flags::gFeatureFlagSearchExtension;
    std::vector<BSONObj> flagValues{BSON("name" << flag.getName() << "value" << false)};
    auto ifrContext = IncrementalFeatureRolloutContext::forTest(flagValues);

    auto spec = fromjson(R"({
        $searchMeta: {
            query: "cakes",
            path: "title"
        }
    })");

    // Should successfully parse using the fallback legacy implementation.
    ASSERT_DOES_NOT_THROW(LiteParsedDocumentSource::parse(
        nss, spec, LiteParserOptions{.ifrContext = ifrContext, .opCtx = opCtx}));
}

// Each internal routing field must be individually rejected when supplied by an external client.
TEST_F(SearchMetaTest, ExternalClientCannotSupplyInternalSearchMetaFields) {
    auto session = transport::MockSession::create(nullptr);
    auto externalClient = getServiceContext()->getService()->makeClient("externalClient", session);
    auto externalOpCtx = externalClient->makeOperationContext();

    auto nss = getExpCtx()->getNamespaceString();
    for (const auto& [fieldName, specJson] : kInternalSearchMetaFieldCases) {
        SCOPED_TRACE(fieldName);
        const auto specBson = fromjson(specJson);
        auto lpds = SearchMetaLiteParsed::parse(nss, specBson.firstElement(), LiteParserOptions{});
        ASSERT_THROWS_CODE(lpds->validate(externalOpCtx.get()), AssertionException, 5491300);
    }
}

// Internal clients (no transport session) must still be able to supply internal routing fields.
// Iterates the same case list as the external-client test so the two stay in sync.
TEST_F(SearchMetaTest, InternalClientCanSupplyInternalSearchMetaFields) {
    auto opCtx = getExpCtx()->getOperationContext();
    auto nss = getExpCtx()->getNamespaceString();

    for (const auto& [fieldName, specJson] : kInternalSearchMetaFieldCases) {
        SCOPED_TRACE(fieldName);
        const auto specBson = fromjson(specJson);
        auto lpds = SearchMetaLiteParsed::parse(nss, specBson.firstElement(), LiteParserOptions{});
        ASSERT_DOES_NOT_THROW(lpds->validate(opCtx));
    }
}

// createFromBson must accept a spec already in its serialized internal ('mongotQuery') form;
// validation of those fields belongs to the LiteParse layer.
TEST_F(SearchMetaTest, CreateFromBsonAcceptsSerializedInternalSpec) {
    auto expCtx = getExpCtx();
    expCtx->setMongoProcessInterface(std::make_unique<MockMongoInterface>());

    auto fromNs = NamespaceString::createNamespaceString_forTest("unittests.$cmd.aggregate");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});

    const auto serializedSpec = fromjson(R"({
        $searchMeta: {
            mongotQuery: {index: "default", text: {query: "hello", path: "body"}, count: {type: "total"}},
            metadataMergeProtocolVersion: 1,
            mergingPipeline: []
        }
    })");

    ASSERT_DOES_NOT_THROW(
        DocumentSourceSearchMeta::createFromBson(serializedSpec.firstElement(), expCtx));
}

}  // namespace
}  // namespace mongo
