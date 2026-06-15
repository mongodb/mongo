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
    ASSERT_EQ(userLpp.getStages()[0]->getParseTimeName(), "$match"_sd);
    ASSERT_EQ(userLpp.getStages()[1]->getParseTimeName(), "$match"_sd);
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
    ASSERT_EQ(userLpp.getStages()[0]->getParseTimeName(), "$unionWith"_sd);

    // Drill into the subpipeline via getMutableSubPipelines.
    auto* subs = userLpp.getStages()[0]->getMutableSubPipelines();
    ASSERT_EQ(subs->size(), 1U);

    // The inner subpipeline should now have 2 stages: view's $match prepended + user's $match.
    const auto& innerStages = subs->front()->getStages();
    ASSERT_EQ(innerStages.size(), 2U);
    ASSERT_EQ(innerStages[0]->getParseTimeName(), "$match"_sd);
    ASSERT_EQ(innerStages[1]->getParseTimeName(), "$match"_sd);

    // The recursive resolver should have bound the resolved subpipeline view onto the parent
    // $unionWith stage so downstream DocumentSource construction can skip a per-stage view
    // application.
    auto resolvedSubPipelineView = userLpp.getStages()[0]->getResolvedSubPipelineView();
    ASSERT_TRUE(resolvedSubPipelineView != nullptr);
    ASSERT_EQ(resolvedSubPipelineView->getNamespace(), kUnionViewNss);
}

TEST(PipelineResolverTest, ResolverDoesNotBindResolvedSubpipelineViewOnNonViewTarget) {
    // $unionWith targeting a regular collection (not a view) should leave the parent stage's
    // resolved-view marker null.
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

    // No view in the map → parent stage should not have a resolved subpipeline view bound.
    ASSERT_TRUE(userLpp.getStages()[0]->getResolvedSubPipelineView() == nullptr);
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
    ASSERT_EQ(userLpp.getStages()[0]->getParseTimeName(), "$_internalSearchIdLookup"_sd);
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
    ASSERT_EQ(outerSubStages[0]->getParseTimeName(), "$match"_sd);
    ASSERT_EQ(outerSubStages[1]->getParseTimeName(), "$unionWith"_sd);

    // Inner $unionWith sub-LPP: [{$match:{v:1}} (from viewMain)] — not empty, proving the
    // second reference to viewMain was resolved and not suppressed as a cycle.
    auto* innerSubs = outerSubStages[1]->getMutableSubPipelines();
    ASSERT_TRUE(innerSubs && innerSubs->size() == 1U);
    const auto& innerSubStages = (*innerSubs)[0]->getStages();
    ASSERT_EQ(innerSubStages.size(), 1U);
    ASSERT_EQ(innerSubStages[0]->getParseTimeName(), "$match"_sd);
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
    ASSERT_EQ(middleStages[0]->getParseTimeName(), "$unionWith"_sd);

    auto* middleSubs = middleStages[0]->getMutableSubPipelines();
    ASSERT_TRUE(middleSubs && middleSubs->size() == 1U);
    const auto& innermostStages = (*middleSubs)[0]->getStages();
    ASSERT_EQ(innermostStages.size(), 1U);
    ASSERT_EQ(innermostStages[0]->getParseTimeName(), "$unionWith"_sd);

    // The innermost $unionWith targeted innerView should be fully resolved.
    auto* innerSubs = innermostStages[0]->getMutableSubPipelines();
    ASSERT_TRUE(innerSubs && innerSubs->size() == 1U);
    const auto& viewStages = (*innerSubs)[0]->getStages();
    ASSERT_EQ(viewStages.size(), 1U);
    ASSERT_EQ(viewStages[0]->getParseTimeName(), "$match"_sd);
}

TEST(PipelineResolverTest, InsertTopLevelViewEntryStoresResolvedView) {
    // Build a ResolvedView backed by kBackingNss.
    BSONObj viewStage = BSON("$match" << BSON("v" << 1));
    std::vector<BSONObj> viewPipeline{viewStage};
    ResolvedView resolvedView(kBackingNss, viewPipeline, BSONObj{});

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
    ASSERT_TRUE(entry.involvedNamespaceIsAView);

    // Assert: backing namespace (entry.ns) matches the ResolvedView's namespace.
    ASSERT_EQ(entry.ns, resolvedView.getNamespace());
}

}  // namespace
}  // namespace mongo
