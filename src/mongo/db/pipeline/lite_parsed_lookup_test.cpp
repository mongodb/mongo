// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/lite_parsed_lookup.h"

#include "mongo/bson/json.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/unittest/unittest.h"

#include <string_view>

namespace mongo {
namespace {

class LiteParsedLookUpTest : public AggregationContextFixture {
protected:
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.local");

    std::unique_ptr<LiteParsedLookUp> parse(std::string_view json,
                                            LiteParserOptions options = LiteParserOptions{}) {
        auto spec = fromjson(json);
        auto result = LiteParsedLookUp::parse(nss, spec.firstElement(), options);
        result->makeOwned();
        return result;
    }

    LookUpStageParams* parseAndGetParams(std::string_view json,
                                         LiteParserOptions options = LiteParserOptions{}) {
        _lastParsed = parse(json, options);
        _lastParams = _lastParsed->getStageParams();
        auto* typed = dynamic_cast<LookUpStageParams*>(_lastParams.get());
        ASSERT_TRUE(typed);
        return typed;
    }

private:
    std::unique_ptr<LiteParsedLookUp> _lastParsed;
    std::unique_ptr<StageParams> _lastParams;
};

TEST_F(LiteParsedLookUpTest, ParsesPipelineOnlyForm) {
    auto lp = parse(R"({$lookup: {from: "foreign", as: "a", pipeline: [{$match: {x: 1}}]}})");
    auto involved = lp->getInvolvedNamespaces();
    ASSERT_EQ(involved.size(), 1u);
    ASSERT_TRUE(involved.contains(NamespaceString::createNamespaceString_forTest("test.foreign")));
}

TEST_F(LiteParsedLookUpTest, ParsesLocalForeignFieldForm) {
    auto lp = parse(R"({$lookup: {from: "foreign", as: "a", localField: "x", foreignField: "y"}})");
    ASSERT_EQ(lp->getInvolvedNamespaces().size(), 1u);
}

TEST_F(LiteParsedLookUpTest, ParsesCrossDbForm) {
    LiteParserOptions opts;
    opts.allowGenericForeignDbLookup = true;
    auto* typed = parseAndGetParams(
        R"({$lookup: {from: {db: "other", coll: "c"}, as: "a", pipeline: []}})", opts);
    ASSERT_TRUE(typed->hasForeignDB);
}

TEST_F(LiteParsedLookUpTest, ParsesCollectionlessForm) {
    auto lp = parse(R"({$lookup: {as: "a", pipeline: [{$documents: [{}]}]}})");
    ASSERT_FALSE(lp->getInvolvedNamespaces().empty());
    ASSERT_TRUE(lp->getInvolvedNamespaces().begin()->isCollectionlessAggregateNS());
}

// ---- Parse failures ----

TEST_F(LiteParsedLookUpTest, RejectsNonObjectSpec) {
    ASSERT_THROWS_CODE(
        parse(R"({$lookup: "just-a-string"})"), AssertionException, ErrorCodes::FailedToParse);
}

TEST_F(LiteParsedLookUpTest, RejectsCollectionlessWithoutDocuments) {
    ASSERT_THROWS_CODE(parse(R"({$lookup: {as: "a", pipeline: [{$match: {}}]}})"),
                       AssertionException,
                       ErrorCodes::FailedToParse);
}

TEST_F(LiteParsedLookUpTest, RejectsMissingPipelineAndLocalForeignFields) {
    ASSERT_THROWS_CODE(parse(R"({$lookup: {from: "foreign", as: "a"}})"),
                       AssertionException,
                       ErrorCodes::FailedToParse);
    ASSERT_THROWS_CODE(parse(R"({$lookup: {from: "foreign", as: "a", localField: "x"}})"),
                       AssertionException,
                       ErrorCodes::FailedToParse);
    ASSERT_THROWS_CODE(parse(R"({$lookup: {from: "foreign", as: "a", foreignField: "y"}})"),
                       AssertionException,
                       ErrorCodes::FailedToParse);
}

TEST_F(LiteParsedLookUpTest, StageParamsCarriesDesugaredPipelineWhenPresent) {
    auto* typed =
        parseAndGetParams(R"({$lookup: {from: "foreign", as: "a", pipeline: [{$match: {}}]}})");
    ASSERT_TRUE(typed->subpipelineStageParams.has_value());
    ASSERT_EQ(typed->subpipelineStageParams->size(), 1U);
    ASSERT_EQ(typed->pipeline.size(), 1u);
    ASSERT_EQ(typed->as, "a");
    ASSERT_FALSE(typed->localField.has_value());
    ASSERT_FALSE(typed->foreignField.has_value());
    ASSERT_FALSE(typed->hasForeignDB);
}

TEST_F(LiteParsedLookUpTest, StageParamsOmitsLppForLocalForeignFieldForm) {
    auto* typed = parseAndGetParams(
        R"({$lookup: {from: "foreign", as: "a", localField: "x", foreignField: "y"}})");
    ASSERT_FALSE(typed->subpipelineStageParams.has_value());
    ASSERT_TRUE(typed->localField.has_value());
    ASSERT_EQ(*typed->localField, "x");
    ASSERT_TRUE(typed->foreignField.has_value());
    ASSERT_EQ(*typed->foreignField, "y");
}

TEST_F(LiteParsedLookUpTest, StageParamsForwardsIsHybridSearchFlag) {
    auto* typed = parseAndGetParams(R"({
        $lookup: {from: "foreign", as: "a", pipeline: [{$match: {}}], $_internalIsHybridSearch: true}
    })");
    ASSERT_TRUE(typed->isHybridSearch);
}

TEST_F(LiteParsedLookUpTest, StageParamsForwardsSubpipelineViewPolicy) {
    // An ordinary subpipeline first stage defaults to kDefaultPrepend.
    auto* typed =
        parseAndGetParams(R"({$lookup: {from: "foreign", as: "a", pipeline: [{$match: {}}]}})");
    ASSERT(typed->subpipelineViewPolicy == FirstStageViewApplicationPolicy::kDefaultPrepend);

    // A first stage that applies the view itself forwards kDoNothing.
    typed = parseAndGetParams(
        R"({$lookup: {from: "foreign", as: "a", pipeline: [{$_internalSearchIdLookup: {}}]}})");
    ASSERT(typed->subpipelineViewPolicy == FirstStageViewApplicationPolicy::kDoNothing);
}

TEST_F(LiteParsedLookUpTest, StageParamsCarriesLetVarsAndUnwindSpec) {
    auto* typed = parseAndGetParams(R"({
        $lookup: {
            from: "foreign", as: "a", pipeline: [{$match: {}}],
            let: {v: "$x", w: "$y"},
            $_internalUnwind: {$unwind: "$a"}
        }
    })");
    ASSERT_BSONOBJ_EQ(typed->letVariables, fromjson(R"({v: "$x", w: "$y"})"));
    ASSERT_TRUE(typed->unwindSpec.has_value());
    ASSERT_BSONOBJ_EQ(*typed->unwindSpec, fromjson(R"({$unwind: "$a"})"));
}

TEST_F(LiteParsedLookUpTest, RequiredPrivilegesIncludesForeignFind) {
    auto lp = parse(R"({$lookup: {from: "foreign", as: "a", pipeline: []}})");
    auto privs = lp->requiredPrivileges(false /*isMongos*/, false /*bypass*/);
    ASSERT_EQ(privs.size(), 1u);
}

TEST_F(LiteParsedLookUpTest, ValidatePassesForSameDatabaseLookup) {
    auto lp = parse(R"({$lookup: {from: "foreign", as: "a", pipeline: [{$match: {x: 1}}]}})");
    ASSERT_DOES_NOT_THROW(lp->validate());
}

TEST_F(LiteParsedLookUpTest, ParseRejectsCrossDbByDefault) {
    ASSERT_THROWS_CODE(
        parse(R"({$lookup: {from: {db: "other", coll: "c"}, as: "a", pipeline: []}})"),
        AssertionException,
        ErrorCodes::FailedToParse);
}

TEST_F(LiteParsedLookUpTest, ParseAllowsCrossDbToConfigCacheChunks) {
    // config.cache.chunks.* is explicitly allowed.
    auto lp = parse(
        R"({$lookup: {from: {db: "config", coll: "cache.chunks.test.foo"}, as: "a", pipeline: []}})");
    ASSERT_DOES_NOT_THROW(lp->validate());
}

TEST_F(LiteParsedLookUpTest, ParseAllowsCrossDbWhenAllowGenericForeignDbLookupSet) {
    LiteParserOptions opts;
    opts.allowGenericForeignDbLookup = true;
    auto lp = parse(R"({$lookup: {from: {db: "other", coll: "c"}, as: "a", pipeline: []}})", opts);
    ASSERT_DOES_NOT_THROW(lp->validate());
}

}  // namespace
}  // namespace mongo
