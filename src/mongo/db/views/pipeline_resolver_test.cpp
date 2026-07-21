// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/views/pipeline_resolver.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/owned_lite_parsed_pipeline.h"
#include "mongo/db/pipeline/resolved_namespace.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

const NamespaceString kUserNss =
    NamespaceString::createNamespaceString_forTest("testdb", "usercoll");
const NamespaceString kBackingNss =
    NamespaceString::createNamespaceString_forTest("testdb", "backing");

TEST(PipelineResolverTest, ResolveInvolvedReturnsFalseOnEmptyMap) {
    // Build an empty LiteParsedPipeline with mainNss=kUserNss.
    LiteParsedPipeline lpp(kUserNss, std::vector<BSONObj>{}, true, LiteParserOptions{});

    // Assert that the LPP has zero stages before the call.
    ASSERT_EQ(lpp.getStages().size(), 0U);

    // Call with an empty ResolvedNamespaceMap — traversal-only semantics.
    ResolvedNamespaceMap emptyMap;
    bool result =
        PipelineResolver::resolveInvolvedNamespacesOnLiteParsedPipeline(&lpp, kUserNss, emptyMap);

    // Assert that the function returns false.
    ASSERT_FALSE(result);

    // Assert that the LPP is unchanged (still zero stages).
    ASSERT_EQ(lpp.getStages().size(), 0U);
}

TEST(PipelineResolverTest, BindsTopLevelView) {
    // View pipeline: [{$match: {v: 1}}]
    BSONObj viewStage = BSON("$match" << BSON("v" << 1));
    std::vector<BSONObj> viewPipeline{viewStage};

    // Build a ResolvedNamespaceMap entry for kUserNss as a view backed by kBackingNss.
    ResolvedNamespaceViewOptions opts;
    opts.involvedNamespaceIsAView = true;
    opts.shouldParseLpp = true;

    ResolvedNamespace rn(kUserNss, kBackingNss, viewPipeline, BSONObj{}, opts);

    ResolvedNamespaceMap nsMap;
    nsMap.emplace(kUserNss, std::move(rn));

    // User pipeline: [{$match: {u: 1}}]
    BSONObj userStage = BSON("$match" << BSON("u" << 1));
    LiteParsedPipeline userLpp(kUserNss, std::vector<BSONObj>{userStage});

    bool result =
        PipelineResolver::resolveInvolvedNamespacesOnLiteParsedPipeline(&userLpp, kUserNss, nsMap);

    ASSERT_TRUE(result);
    ASSERT_EQ(userLpp.getStages().size(), 2U);
    // Both stages should be $match.
    ASSERT_EQ(userLpp.getStages()[0]->getParseTimeName(), "$match"sv);
    ASSERT_EQ(userLpp.getStages()[1]->getParseTimeName(), "$match"sv);
}

TEST(PipelineResolverTest, RecursesIntoUnionWithSubpipeline) {
    const NamespaceString kLocalUserNss =
        NamespaceString::createNamespaceString_forTest("test", "userColl");
    const NamespaceString kUnionViewNss =
        NamespaceString::createNamespaceString_forTest("test", "unionView");
    const NamespaceString kUnionBackingNss =
        NamespaceString::createNamespaceString_forTest("test", "unionBacking");

    // View pipeline for the unionView: [{$match: {v: 1}}]
    BSONObj viewMatchStage = BSON("$match" << BSON("v" << 1));
    std::vector<BSONObj> viewPipeline{viewMatchStage};

    // Build ResolvedNamespaceMap: kUnionViewNss is a view backed by kUnionBackingNss.
    // kLocalUserNss is NOT in the map (plain collection).
    ResolvedNamespaceViewOptions opts;
    opts.involvedNamespaceIsAView = true;
    opts.shouldParseLpp = true;

    ResolvedNamespace rnView(kUnionViewNss, kUnionBackingNss, viewPipeline, BSONObj{}, opts);
    ResolvedNamespaceMap nsMap;
    nsMap.emplace(kUnionViewNss, std::move(rnView));

    // User pipeline: [{$unionWith: {coll: "unionView", pipeline: [{$match: {i: 1}}]}}]
    BSONObj innerMatchStage = BSON("$match" << BSON("i" << 1));
    BSONObj unionWithSpec = BSON("coll" << "unionView"
                                        << "pipeline" << BSON_ARRAY(innerMatchStage));
    BSONObj userTopStage = BSON("$unionWith" << unionWithSpec);
    LiteParsedPipeline userLpp(
        kLocalUserNss, std::vector<BSONObj>{userTopStage}, true, LiteParserOptions{});

    // Call the resolver — kLocalUserNss is NOT a view, so top-level should not be bound.
    bool result = PipelineResolver::resolveInvolvedNamespacesOnLiteParsedPipeline(
        &userLpp, kLocalUserNss, nsMap);

    // The function should return true because a nested view was bound.
    ASSERT_TRUE(result);

    // Top-level stage count should remain 1 (still just $unionWith).
    ASSERT_EQ(userLpp.getStages().size(), 1U);
    ASSERT_EQ(userLpp.getStages()[0]->getParseTimeName(), "$unionWith"sv);

    // Drill into the subpipeline via getMutableSubPipelines.
    auto* subs = userLpp.getStages()[0]->getMutableSubPipelines();
    ASSERT_EQ(subs->size(), 1U);

    // The inner subpipeline should now have 2 stages: view's $match prepended + user's $match.
    const auto& innerStages = subs->front()->getStages();
    ASSERT_EQ(innerStages.size(), 2U);
    ASSERT_EQ(innerStages[0]->getParseTimeName(), "$match"sv);
    ASSERT_EQ(innerStages[1]->getParseTimeName(), "$match"sv);

    // The recursive resolver should have upgraded the resolved backing namespace on the parent
    // $unionWith stage to the view, so downstream DocumentSource construction can skip a per-stage
    // view application.
    auto resolvedBackingNss = userLpp.getStages()[0]->getResolvedBackingNss();
    ASSERT_TRUE(resolvedBackingNss.isInvolvedNamespaceAView());
    ASSERT_EQ(resolvedBackingNss.getNamespace(), kUnionViewNss);
}

TEST(PipelineResolverTest, ResolverDoesNotBindResolvedSubpipelineViewOnNonViewTarget) {
    // $unionWith targeting a regular collection (not a view) should leave the parent stage's
    // resolved backing namespace at its identity default (not a view).
    const NamespaceString kLocalUserNss =
        NamespaceString::createNamespaceString_forTest("test", "userColl");
    const NamespaceString kForeignNss =
        NamespaceString::createNamespaceString_forTest("test", "foreignColl");

    ResolvedNamespaceMap emptyMap;

    BSONObj userTopStage = BSON("$unionWith" << BSON("coll" << "foreignColl"
                                                            << "pipeline" << BSONArray()));
    LiteParsedPipeline userLpp(
        kLocalUserNss, std::vector<BSONObj>{userTopStage}, true, LiteParserOptions{});

    PipelineResolver::resolveInvolvedNamespacesOnLiteParsedPipeline(
        &userLpp, kLocalUserNss, emptyMap);

    // No view in the map → parent stage's resolved backing namespace stays at its identity default:
    // not a view, and pointing at the user-provided foreign namespace.
    auto resolvedBackingNss = userLpp.getStages()[0]->getResolvedBackingNss();
    ASSERT_FALSE(resolvedBackingNss.isInvolvedNamespaceAView());
    ASSERT_EQ(resolvedBackingNss.getNamespace(), kForeignNss);
}

TEST(PipelineResolverTest, WalkerRespectsViewPolicyKDoNothing) {
    // kViewNss is a view backed by kBackingNss with a plain $match stage.
    const NamespaceString kViewNss =
        NamespaceString::createNamespaceString_forTest("test", "searchView");
    const NamespaceString kBacking =
        NamespaceString::createNamespaceString_forTest("test", "backing");

    // View pipeline: [{$match: {k: 1}}]
    BSONObj viewStage = BSON("$match" << BSON("k" << 1));
    std::vector<BSONObj> viewPipeline{viewStage};

    ResolvedNamespaceViewOptions opts;
    opts.involvedNamespaceIsAView = true;
    opts.shouldParseLpp = true;

    ResolvedNamespace rn(kViewNss, kBacking, viewPipeline, BSONObj{}, opts);

    ResolvedNamespaceMap map;
    map.emplace(kViewNss, std::move(rn));

    // User pipeline: [{$_internalSearchIdLookup: {}}]
    // $_internalSearchIdLookup has getFirstStageViewApplicationPolicy() == kDoNothing,
    // so handleView() must NOT prepend the view's $match stage.
    BSONObj userStage = BSON("$_internalSearchIdLookup" << BSONObj());
    LiteParsedPipeline userLpp(
        kViewNss, std::vector<BSONObj>{userStage}, true, LiteParserOptions{});

    // Sanity: user LPP has exactly 1 stage before the call.
    ASSERT_EQ(userLpp.getStages().size(), 1U);

    // Call the resolver — kViewNss IS a view, but the user LPP's first stage vetoes prepending.
    // The return value may be true (view found) or false depending on implementation; we don't
    // constrain it here.
    PipelineResolver::resolveInvolvedNamespacesOnLiteParsedPipeline(&userLpp, kViewNss, map);

    // Assert: stages must NOT have been prepended — still exactly 1 stage.
    ASSERT_EQ(userLpp.getStages().size(), 1U);
    ASSERT_EQ(userLpp.getStages()[0]->getParseTimeName(), "$_internalSearchIdLookup"sv);
}

TEST(PipelineResolverTest, AllowsRepeatedViewReferenceInUserStages) {
    // Case A from resolveInvolvedNamespacesImpl: when mainNss is a view, user-written stages
    // may legitimately reference the same view again. The resolver lifts mainNss from
    // `inProgress` while walking user stages so those references are not suppressed.
    //
    // Setup: aggregate on plain userColl with
    //   [{$unionWith: {coll: "viewMain", pipeline:
    //       [{$unionWith: {coll: "viewMain", pipeline: []}}]}}]
    // where viewMain has pipeline [{$match: {v: 1}}].
    //
    // Expected: both the outer and inner $unionWith{viewMain} sub-LPPs get viewMain's $match
    // prepended, proving cycle-detection did not suppress the second reference.
    const NamespaceString kViewNss =
        NamespaceString::createNamespaceString_forTest("testdb", "viewMain");
    const NamespaceString kViewBackingNss =
        NamespaceString::createNamespaceString_forTest("testdb", "backingMain");
    const NamespaceString kUserCollNss =
        NamespaceString::createNamespaceString_forTest("testdb", "userColl");

    BSONObj viewMatchStage = BSON("$match" << BSON("v" << 1));
    std::vector<BSONObj> viewPipeline{viewMatchStage};

    ResolvedNamespaceViewOptions opts;
    opts.involvedNamespaceIsAView = true;
    opts.shouldParseLpp = true;

    ResolvedNamespaceMap nsMap;
    nsMap.emplace(kViewNss,
                  ResolvedNamespace(kViewNss, kViewBackingNss, viewPipeline, BSONObj{}, opts));

    BSONObj innerUnionWith = BSON("$unionWith" << BSON("coll" << "viewMain"
                                                              << "pipeline" << BSONArray()));
    BSONObj outerUnionWith =
        BSON("$unionWith" << BSON("coll" << "viewMain"
                                         << "pipeline" << BSON_ARRAY(innerUnionWith)));
    LiteParsedPipeline userLpp(
        kUserCollNss, std::vector<BSONObj>{outerUnionWith}, true, LiteParserOptions{});

    bool result = PipelineResolver::resolveInvolvedNamespacesOnLiteParsedPipeline(
        &userLpp, kUserCollNss, nsMap);
    ASSERT_TRUE(result);

    // Top level: still just the one outer $unionWith.
    ASSERT_EQ(userLpp.getStages().size(), 1U);

    // Outer $unionWith sub-LPP: [{$match:{v:1}} (from viewMain), {$unionWith: viewMain}].
    auto* outerSubs = userLpp.getStages()[0]->getMutableSubPipelines();
    ASSERT_TRUE(outerSubs && outerSubs->size() == 1U);
    const auto& outerSubStages = (*outerSubs)[0]->getStages();
    ASSERT_EQ(outerSubStages.size(), 2U);
    ASSERT_EQ(outerSubStages[0]->getParseTimeName(), "$match"sv);
    ASSERT_EQ(outerSubStages[1]->getParseTimeName(), "$unionWith"sv);

    // Inner $unionWith sub-LPP: [{$match:{v:1}} (from viewMain)] — not empty, proving the
    // second reference to viewMain was resolved and not suppressed as a cycle.
    auto* innerSubs = outerSubStages[1]->getMutableSubPipelines();
    ASSERT_TRUE(innerSubs && innerSubs->size() == 1U);
    const auto& innerSubStages = (*innerSubs)[0]->getStages();
    ASSERT_EQ(innerSubStages.size(), 1U);
    ASSERT_EQ(innerSubStages[0]->getParseTimeName(), "$match"sv);
}

TEST(PipelineResolverTest, ResolvesInnerViewNestedUnderRepeatedBaseCollectionReferences) {
    const NamespaceString kUserCollNss =
        NamespaceString::createNamespaceString_forTest("testdb", "userColl");
    const NamespaceString kInnerViewNss =
        NamespaceString::createNamespaceString_forTest("testdb", "innerView");
    const NamespaceString kInnerBackingNss =
        NamespaceString::createNamespaceString_forTest("testdb", "innerBacking");

    BSONObj viewMatchStage = BSON("$match" << BSON("v" << 1));
    std::vector<BSONObj> viewPipeline{viewMatchStage};

    ResolvedNamespaceViewOptions opts;
    opts.involvedNamespaceIsAView = true;
    opts.shouldParseLpp = true;

    ResolvedNamespaceMap nsMap;
    nsMap.emplace(
        kInnerViewNss,
        ResolvedNamespace(kInnerViewNss, kInnerBackingNss, viewPipeline, BSONObj{}, opts));

    BSONObj innermost = BSON("$unionWith" << BSON("coll" << "innerView"
                                                         << "pipeline" << BSONArray()));
    BSONObj middle = BSON("$unionWith" << BSON("coll" << "userColl"
                                                      << "pipeline" << BSON_ARRAY(innermost)));
    BSONObj outer = BSON("$unionWith" << BSON("coll" << "userColl"
                                                     << "pipeline" << BSON_ARRAY(middle)));
    LiteParsedPipeline userLpp(
        kUserCollNss, std::vector<BSONObj>{outer}, true, LiteParserOptions{});

    bool result = PipelineResolver::resolveInvolvedNamespacesOnLiteParsedPipeline(
        &userLpp, kUserCollNss, nsMap);

    ASSERT_TRUE(result);

    // Walk outer -> middle: neither targets a view, so nothing is prepended at those levels.
    ASSERT_EQ(userLpp.getStages().size(), 1U);
    auto* outerSubs = userLpp.getStages()[0]->getMutableSubPipelines();
    ASSERT_TRUE(outerSubs && outerSubs->size() == 1U);
    const auto& middleStages = (*outerSubs)[0]->getStages();
    ASSERT_EQ(middleStages.size(), 1U);
    ASSERT_EQ(middleStages[0]->getParseTimeName(), "$unionWith"sv);

    auto* middleSubs = middleStages[0]->getMutableSubPipelines();
    ASSERT_TRUE(middleSubs && middleSubs->size() == 1U);
    const auto& innermostStages = (*middleSubs)[0]->getStages();
    ASSERT_EQ(innermostStages.size(), 1U);
    ASSERT_EQ(innermostStages[0]->getParseTimeName(), "$unionWith"sv);

    // The innermost $unionWith targeted innerView should be fully resolved.
    auto* innerSubs = innermostStages[0]->getMutableSubPipelines();
    ASSERT_TRUE(innerSubs && innerSubs->size() == 1U);
    const auto& viewStages = (*innerSubs)[0]->getStages();
    ASSERT_EQ(viewStages.size(), 1U);
    ASSERT_EQ(viewStages[0]->getParseTimeName(), "$match"sv);
}

TEST(PipelineResolverTest, DoesNotInfiniteLoopOnMutualGraphLookupViewCycle) {
    // viewB has pipeline [{$graphLookup: {from: "viewE", ...}}]
    // viewE has pipeline [{$graphLookup: {from: "viewB", ...}}]
    // Both back the same base collection. Before the fix, resolveInvolvedNamespacesImpl would
    // recurse infinitely because the materialized sub-pipeline uses the backing-collection NSS
    // (not the view NSS), bypassing the inProgress cycle-detection guard.
    const NamespaceString kViewBNss =
        NamespaceString::createNamespaceString_forTest("testdb", "viewB");
    const NamespaceString kViewENss =
        NamespaceString::createNamespaceString_forTest("testdb", "viewE");
    const NamespaceString kBaseColl =
        NamespaceString::createNamespaceString_forTest("testdb", "fsmcoll0");

    auto makeGraphLookupStage = [](std::string_view fromColl) {
        return BSON("$graphLookup" << BSON("from" << fromColl << "startWith"
                                                  << "$a"
                                                  << "connectFromField"
                                                  << "a"
                                                  << "connectToField"
                                                  << "b"
                                                  << "as"
                                                  << "result"));
    };

    // viewB: {viewOn: "fsmcoll0", pipeline: [{$graphLookup: {from: "viewE", ...}}]}
    std::vector<BSONObj> viewBPipeline{makeGraphLookupStage("viewE")};
    // viewE: {viewOn: "fsmcoll0", pipeline: [{$graphLookup: {from: "viewB", ...}}]}
    std::vector<BSONObj> viewEPipeline{makeGraphLookupStage("viewB")};

    ResolvedNamespaceViewOptions opts;
    opts.involvedNamespaceIsAView = true;
    opts.shouldParseLpp = true;

    ResolvedNamespaceMap nsMap;
    nsMap.emplace(kViewBNss,
                  ResolvedNamespace(kViewBNss, kBaseColl, viewBPipeline, BSONObj{}, opts));
    nsMap.emplace(kViewENss,
                  ResolvedNamespace(kViewENss, kBaseColl, viewEPipeline, BSONObj{}, opts));

    // User queries viewB (empty pipeline, like a find command).
    LiteParsedPipeline userLpp(kViewBNss, std::vector<BSONObj>{}, true, LiteParserOptions{});

    // This must complete without hanging or crashing (stack overflow).
    bool result =
        PipelineResolver::resolveInvolvedNamespacesOnLiteParsedPipeline(&userLpp, kViewBNss, nsMap);

    // A view was bound (viewB's pipeline was prepended).
    ASSERT_TRUE(result);

    // viewB's $graphLookup stage was prepended — exactly one stage.
    ASSERT_EQ(userLpp.getStages().size(), 1U);
    ASSERT_EQ(userLpp.getStages()[0]->getParseTimeName(), "$graphLookup"sv);
}

TEST(PipelineResolverTest, MongotLedPipelineOnViewBindsUnionWithSubpipelineView) {
    const NamespaceString kViewNss =
        NamespaceString::createNamespaceString_forTest("test", "searchView");
    const NamespaceString kViewBackingNss =
        NamespaceString::createNamespaceString_forTest("test", "searchBacking");

    // Insert the top-level view entry the way maybeApplyViewPipeline does for view aggregations.
    BSONObj viewMatchStage = BSON("$match" << BSON("v" << 1));
    ResolvedNamespaceViewOptions opts;
    opts.involvedNamespaceIsAView = true;
    ResolvedNamespace rnView(kViewNss, kViewBackingNss, {viewMatchStage}, BSONObj{}, opts);
    ResolvedNamespaceMap nsMap;
    PipelineResolver::insertTopLevelViewEntry(nsMap, kViewNss, std::move(rnView));

    // Model the shape hybrid search desugars to on a view: a mongot stage first, followed by a
    // $unionWith wrapping another input pipeline that targets the same view.
    BSONObj searchStage = BSON(
        "$search" << BSON("index" << "idx" << "text" << BSON("query" << "q" << "path" << "m")));
    BSONObj unionStage = BSON(
        "$unionWith" << BSON("coll" << "searchView"
                                    << "pipeline" << BSON_ARRAY(BSON("$match" << BSON("i" << 1)))));
    LiteParsedPipeline userLpp(
        kViewNss, std::vector<BSONObj>{searchStage, unionStage}, true, LiteParserOptions{});

    bool result =
        PipelineResolver::resolveInvolvedNamespacesOnLiteParsedPipeline(&userLpp, kViewNss, nsMap);
    ASSERT_TRUE(result);

    // Mongot-led pipelines are bind-only at the top level: the view pipeline must NOT be
    // prepended (mongot applies the view itself).
    ASSERT_EQ(userLpp.getStages().size(), 2U);
    ASSERT_EQ(userLpp.getStages()[0]->getParseTimeName(), "$search"sv);
    ASSERT_EQ(userLpp.getStages()[1]->getParseTimeName(), "$unionWith"sv);

    // The $unionWith must be marked view-resolved so DocumentSource construction keeps the
    // parsed stage params instead of reparsing the sub-pipeline from BSON (which drops AST-only
    // extension stage specs).
    auto resolvedBackingNss = userLpp.getStages()[1]->getResolvedBackingNss();
    ASSERT_TRUE(resolvedBackingNss.isInvolvedNamespaceAView());
    ASSERT_EQ(resolvedBackingNss.getNamespace(), kViewNss);

    // The recursion must also have handled the view inside the sub-pipeline: the sub-pipeline is
    // not mongot-led, so the view's $match gets stitched in front of the user's $match.
    auto* subs = userLpp.getStages()[1]->getMutableSubPipelines();
    ASSERT_EQ(subs->size(), 1U);
    const auto& innerStages = subs->front()->getStages();
    ASSERT_EQ(innerStages.size(), 2U);
    ASSERT_EQ(innerStages[0]->getParseTimeName(), "$match"sv);
    ASSERT_EQ(innerStages[1]->getParseTimeName(), "$match"sv);
}

TEST(PipelineResolverTest, InsertTopLevelViewEntryStoresResolvedView) {
    // Build a ResolvedNamespace representing a view backed by kBackingNss.
    BSONObj viewStage = BSON("$match" << BSON("v" << 1));
    std::vector<BSONObj> viewPipeline{viewStage};
    ResolvedNamespaceViewOptions opts;
    opts.involvedNamespaceIsAView = true;
    ResolvedNamespace resolvedView(kUserNss, kBackingNss, viewPipeline, BSONObj{}, opts);

    // Build an empty ResolvedNamespaceMap.
    ResolvedNamespaceMap map;

    // Call insertTopLevelViewEntry.
    PipelineResolver::insertTopLevelViewEntry(map, kUserNss, resolvedView);

    // Assert: map has exactly one entry keyed by kUserNss.
    ASSERT_EQ(map.size(), 1U);
    auto it = map.find(kUserNss);
    ASSERT_NE(it, map.end());

    const auto& entry = it->second;

    // Assert: entry's pipeline has one stage (same as resolved view).
    ASSERT_EQ(entry.getBsonPipeline().size(), 1U);

    // Assert: involvedNamespaceIsAView is true.
    ASSERT_TRUE(entry.isInvolvedNamespaceAView());

    // Assert: backing namespace (entry.ns) matches the ResolvedNamespace's resolved namespace.
    ASSERT_EQ(entry.getResolvedNamespace(), resolvedView.getResolvedNamespace());
}

}  // namespace
}  // namespace mongo
