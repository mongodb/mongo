/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/pipeline/lite_parsed_lookup.h"

#include "mongo/bson/json.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class LiteParsedLookUpTest : public AggregationContextFixture {
protected:
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.local");

    std::unique_ptr<LiteParsedLookUp> parse(StringData json,
                                            LiteParserOptions options = LiteParserOptions{}) {
        auto spec = fromjson(json);
        return LiteParsedLookUp::parse(nss, spec.firstElement(), options);
    }

    LookUpStageParams* parseAndGetParams(StringData json,
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

TEST_F(LiteParsedLookUpTest, StageParamsCarriesDesugaredPipelineWhenPresent) {
    auto* typed =
        parseAndGetParams(R"({$lookup: {from: "foreign", as: "a", pipeline: [{$match: {}}]}})");
    ASSERT_TRUE(typed->liteParsedPipeline.has_value());
    ASSERT_EQ(typed->pipeline.size(), 1u);
    ASSERT_EQ(typed->as, "a");
    ASSERT_FALSE(typed->localField.has_value());
    ASSERT_FALSE(typed->foreignField.has_value());
    ASSERT_FALSE(typed->hasForeignDB);
}

TEST_F(LiteParsedLookUpTest, StageParamsOmitsLppForLocalForeignFieldForm) {
    auto* typed = parseAndGetParams(
        R"({$lookup: {from: "foreign", as: "a", localField: "x", foreignField: "y"}})");
    ASSERT_FALSE(typed->liteParsedPipeline.has_value());
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

}  // namespace
}  // namespace mongo
