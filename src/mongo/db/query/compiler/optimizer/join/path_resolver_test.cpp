/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/query/compiler/optimizer/join/path_resolver.h"

#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/query/compiler/optimizer/join/agg_join_model_fixture.h"
#include "mongo/unittest/unittest.h"

namespace mongo::join_ordering {

bool mainCollPathAlwaysScalar(std::string_view path) {
    return false;
}

void validatePath(boost::optional<PathId> id,
                  PathResolver& pr,
                  const FieldPath& expectedPath,
                  NodeId expectedNodeId) {
    ASSERT_NE(id, boost::none);
    ASSERT_LT(*id, pr.resolvedPaths().size());
    const auto& path = pr.resolvedPaths()[*id];
    ASSERT_EQ(path.nodeId, expectedNodeId);
    ASSERT_EQ(path.fieldName.fullPath(), expectedPath);
}

// Resolve, validate actual path value, then verify that a second resolution yields the same path
// id. Ensures that the resolved path has the same field & is set to the expected node.
void testResolve(PathResolver& pr,
                 const FieldPath& field,
                 const FieldPath& expectedPath,
                 const NodeId expectedNode,
                 const DocumentSource* at = nullptr,
                 boost::optional<NodeId> nodeId = boost::none) {
    auto pathId = pr.resolve(field, at, nodeId);
    validatePath(pathId, pr, expectedPath, expectedNode);
    ASSERT_EQ(pr.resolve(field, at, nodeId), pathId);
}

class PathResolverTest : public AggJoinModelFixture {
public:
    std::unique_ptr<Pipeline> makeBasicTestPipeline(std::string_view lf1,
                                                    std::string_view ff1,
                                                    std::string_view as1,
                                                    std::string_view lf2,
                                                    std::string_view ff2,
                                                    std::string_view as2,
                                                    std::string_view prefix = "") {
        auto query =
            std::vformat(R"([
            {}
            {{
                $lookup: {{
                    from: "foreign",
                    localField: "{}",
                    foreignField: "{}",
                    as: "{}"
                }}
            }},
            {{ $unwind: "${}" }},
            {{
                $lookup: {{
                    from: "foreign2",
                    localField: "{}",
                    foreignField: "{}",
                    as: "{}"
                }}
            }},
            {{ $unwind: "${}" }}
            ])",
                         std::make_format_args(prefix, lf1, ff1, as1, as1, lf2, ff2, as2, as2));
        return makePipeline(std::string_view(query), {"foreign", "foreign2"});
    }

    std::unique_ptr<Pipeline> makeSubpipelineTestPipeline(std::string_view let,
                                                          std::string_view subpipeline,
                                                          std::string_view as1,
                                                          std::string_view lf2,
                                                          std::string_view ff2,
                                                          std::string_view as2) {
        auto query = std::vformat(
            R"([
            {{
                $lookup: {{
                    from: "foreign",
                    let: {},
                    pipeline: {},
                    as: "{}"
                }}
            }},
            {{ $unwind: "${}" }},
            {{
                $lookup: {{
                    from: "foreign2",
                    localField: "{}",
                    foreignField: "{}",
                    as: "{}"
                }}
            }},
            {{ $unwind: "${}" }}
            ])",
            std::make_format_args(let, subpipeline, as1, as1, lf2, ff2, as2, as2));
        return makePipeline(std::string_view(query), {"foreign", "foreign2"});
    }
};

TEST_F(PathResolverTest, Basic) {
    auto pipeline = makeBasicTestPipeline("lf", "ff", "embed", "lf2", "ff2", "embed2");
    pipeline::dependency_graph::DependencyGraph dg(pipeline->getSources(),
                                                   mainCollPathAlwaysScalar);

    const NodeId kBaseNode = 0;
    const NodeId kForeignNode = 1;

    PathResolver pr(kBaseNode, dg);

    auto it = pipeline->getSources().begin();
    const auto* dsLookup = it->get();
    const auto* lookup = dynamic_cast<const DocumentSourceLookUp*>(dsLookup);
    ASSERT(lookup);
    ASSERT_TRUE(pr.trackEmbedPath(*lookup, kForeignNode));

    // The scope of the local field is the main pipeline, so we don't specify a
    // node.
    testResolve(pr, "lf", "lf", kBaseNode, dsLookup);

    // The scope of the foreign field must be the "foreign" node. No subpipeline
    // => we can pass nullptr.
    testResolve(pr, "ff", "ff", kForeignNode, nullptr, /* scope */ kForeignNode);

    // Or the same $lookup with a different node.
    const NodeId kForeignNode2 = 2;
    ASSERT_FALSE(pr.trackEmbedPath(*lookup, kForeignNode2));

    // Or a new $lookup with an existing node.
    const auto* dsLookup2 = (++it)->get();
    const auto* lookup2 = dynamic_cast<const DocumentSourceLookUp*>(dsLookup2);
    ASSERT(lookup2);
    ASSERT_FALSE(pr.trackEmbedPath(*lookup2, kBaseNode));
    ASSERT_FALSE(pr.trackEmbedPath(*lookup2, kForeignNode));
    ASSERT(pr.trackEmbedPath(*lookup2, kForeignNode2));

    // Validate that fields not mentioned in the pipeline resolve to the same node as the scope, or
    // base node if no scope is provided.
    testResolve(pr, "a", "a", kForeignNode, nullptr, /* scope */ kForeignNode);
    testResolve(pr, "b", "b", kBaseNode, nullptr, /* scope */ kBaseNode);
    testResolve(pr, "c", "c", kBaseNode, nullptr);

    // Verify that fields from an embed field resolve to the node that provides the embedding.
    testResolve(pr, "embed.a", "a", kForeignNode, nullptr);
    testResolve(pr, "embed2.embed.a", "embed.a", /* scope */ kForeignNode2, nullptr);

    // Verify that if we are within a specified scope, we don't resolve the field to an embedding.
    testResolve(pr, "embed.a", "embed.a", kForeignNode, nullptr, /* scope */ kForeignNode);

    // Validate earlier fields still resolve to same thing.
    testResolve(pr, "lf", "lf", kBaseNode, dsLookup);
    testResolve(pr, "ff", "ff", kForeignNode, nullptr, /* scope */ kForeignNode);
}

TEST_F(PathResolverTest, EmbedPathCollisionExact) {
    auto pipeline = makeBasicTestPipeline("lf", "ff", "as", "lf2", "ff2", "as");
    pipeline::dependency_graph::DependencyGraph dg(pipeline->getSources(),
                                                   mainCollPathAlwaysScalar);
    PathResolver pr(0, dg);

    // No issues embedding our first path.
    auto it = pipeline->getSources().begin();
    const auto* dsLookup = it->get();
    const auto* lookup = dynamic_cast<const DocumentSourceLookUp*>(dsLookup);
    ASSERT(lookup);
    ASSERT_TRUE(pr.trackEmbedPath(*lookup, 1));

    // Second path results in a collision!
    const auto* dsLookup2 = (++it)->get();
    const auto* lookup2 = dynamic_cast<const DocumentSourceLookUp*>(dsLookup2);
    ASSERT(lookup2);
    ASSERT_FALSE(pr.trackEmbedPath(*lookup2, 2));
}

TEST_F(PathResolverTest, EmbedPathCollision2ndPrefixed) {
    auto pipeline = makeBasicTestPipeline("lf", "ff", "as", "lf2", "ff2", "as.bar");
    pipeline::dependency_graph::DependencyGraph dg(pipeline->getSources(),
                                                   mainCollPathAlwaysScalar);
    PathResolver pr(0, dg);

    // No issues embedding our first path.
    auto it = pipeline->getSources().begin();
    const auto* dsLookup = it->get();
    const auto* lookup = dynamic_cast<const DocumentSourceLookUp*>(dsLookup);
    ASSERT(lookup);
    ASSERT_TRUE(pr.trackEmbedPath(*lookup, 1));

    // Second path results in a collision!
    const auto* dsLookup2 = (++it)->get();
    const auto* lookup2 = dynamic_cast<const DocumentSourceLookUp*>(dsLookup2);
    ASSERT(lookup2);
    ASSERT_FALSE(pr.trackEmbedPath(*lookup2, 2));
}

TEST_F(PathResolverTest, EmbedPathCollisions1stPrefixed) {
    auto pipeline = makeBasicTestPipeline("lf", "ff", "as.foo", "lf2", "ff2", "as");
    pipeline::dependency_graph::DependencyGraph dg(pipeline->getSources(),
                                                   mainCollPathAlwaysScalar);
    PathResolver pr(0, dg);

    // No issues embedding our first path.
    auto it = pipeline->getSources().begin();
    const auto* dsLookup = it->get();
    const auto* lookup = dynamic_cast<const DocumentSourceLookUp*>(dsLookup);
    ASSERT(lookup);
    ASSERT_TRUE(pr.trackEmbedPath(*lookup, 1));

    // Second path results in a collision!
    const auto* dsLookup2 = (++it)->get();
    const auto* lookup2 = dynamic_cast<const DocumentSourceLookUp*>(dsLookup2);
    ASSERT(lookup2);
    ASSERT_FALSE(pr.trackEmbedPath(*lookup2, 2));
}

TEST_F(PathResolverTest, EmbedPathShadowsExact) {
    auto pipeline = makeBasicTestPipeline("lf", "ff", "as", "lf2", "ff2", "lf");
    pipeline::dependency_graph::DependencyGraph dg(pipeline->getSources(),
                                                   mainCollPathAlwaysScalar);
    PathResolver pr(0, dg);

    // No issues embedding our first path.
    auto it = pipeline->getSources().begin();
    const auto* dsLookup = it->get();
    const auto* lookup = dynamic_cast<const DocumentSourceLookUp*>(dsLookup);
    ASSERT(lookup);
    ASSERT_TRUE(pr.trackEmbedPath(*lookup, 1));

    // Need to resolve some fields to detect a collision.
    testResolve(pr, "lf", "lf", 0, dsLookup);
    testResolve(pr, "ff", "ff", 1, nullptr, 1);

    // Second path results in a collision!
    const auto* dsLookup2 = (++it)->get();
    const auto* lookup2 = dynamic_cast<const DocumentSourceLookUp*>(dsLookup2);
    ASSERT(lookup2);
    ASSERT_FALSE(pr.trackEmbedPath(*lookup2, 2));
}

TEST_F(PathResolverTest, EmbedPathShadowsPrefix) {
    auto pipeline = makeBasicTestPipeline("lf", "ff", "as", "lf2", "ff2", "lf.as");
    pipeline::dependency_graph::DependencyGraph dg(pipeline->getSources(),
                                                   mainCollPathAlwaysScalar);
    PathResolver pr(0, dg);

    // No issues embedding our first path.
    auto it = pipeline->getSources().begin();
    const auto* dsLookup = it->get();
    const auto* lookup = dynamic_cast<const DocumentSourceLookUp*>(dsLookup);
    ASSERT(lookup);
    ASSERT_TRUE(pr.trackEmbedPath(*lookup, 1));

    // Need to resolve some fields to detect a collision.
    testResolve(pr, "lf", "lf", 0, dsLookup);
    testResolve(pr, "ff", "ff", 1, nullptr, 1);

    // Second path results in a collision!
    const auto* dsLookup2 = (++it)->get();
    const auto* lookup2 = dynamic_cast<const DocumentSourceLookUp*>(dsLookup2);
    ASSERT(lookup2);
    ASSERT_FALSE(pr.trackEmbedPath(*lookup2, 2));
}

TEST_F(PathResolverTest, EmbedPathShadowsSuffix) {
    auto pipeline = makeBasicTestPipeline("lf", "ff", "as.x", "lf2", "ff2", "as");
    pipeline::dependency_graph::DependencyGraph dg(pipeline->getSources(),
                                                   mainCollPathAlwaysScalar);
    PathResolver pr(0, dg);

    // No issues embedding our first path.
    auto it = pipeline->getSources().begin();
    const auto* dsLookup = it->get();
    const auto* lookup = dynamic_cast<const DocumentSourceLookUp*>(dsLookup);
    ASSERT(lookup);
    ASSERT_TRUE(pr.trackEmbedPath(*lookup, 1));

    // Need to resolve some fields to detect a collision.
    testResolve(pr, "lf", "lf", 0, dsLookup);
    testResolve(pr, "ff", "ff", 1, nullptr, 1);

    // Second path results in a collision!
    const auto* dsLookup2 = (++it)->get();
    const auto* lookup2 = dynamic_cast<const DocumentSourceLookUp*>(dsLookup2);
    ASSERT(lookup2);
    ASSERT_FALSE(pr.trackEmbedPath(*lookup2, 2));
}

TEST_F(PathResolverTest, EmbedPathParallelOk) {
    auto pipeline = makeBasicTestPipeline("lf", "ff", "as.x", "lf2", "ff2", "as.y");
    pipeline::dependency_graph::DependencyGraph dg(pipeline->getSources(),
                                                   mainCollPathAlwaysScalar);
    PathResolver pr(0, dg);

    // No issues embedding our first path.
    auto it = pipeline->getSources().begin();
    const auto* dsLookup = it->get();
    const auto* lookup = dynamic_cast<const DocumentSourceLookUp*>(dsLookup);
    ASSERT(lookup);
    ASSERT_TRUE(pr.trackEmbedPath(*lookup, 1));

    // Need to resolve some fields to detect a collision.
    testResolve(pr, "lf", "lf", 0, dsLookup);
    testResolve(pr, "ff", "ff", 1, nullptr, 1);

    // Second path also fine!
    const auto* dsLookup2 = (++it)->get();
    const auto* lookup2 = dynamic_cast<const DocumentSourceLookUp*>(dsLookup2);
    ASSERT(lookup2);
    ASSERT_TRUE(pr.trackEmbedPath(*lookup2, 2));
}

TEST_F(PathResolverTest, HandleSubpipelineProject) {
    auto pipeline = makeSubpipelineTestPipeline(
        R"({aaa: "$lf"})",
        R"([{$match: {$expr: ["$$aaa", "$someField"]}}, {$project: {someField: 1, someOtherField: 1}}])",
        "embed",
        "lf",
        "ff",
        "embed2");
    pipeline::dependency_graph::DependencyGraph dg(pipeline->getSources(),
                                                   mainCollPathAlwaysScalar);
    PathResolver pr(0, dg);

    // No issues embedding our first path.
    auto it = pipeline->getSources().begin();
    const auto* dsLookup = it->get();
    const auto* lookup = dynamic_cast<const DocumentSourceLookUp*>(dsLookup);
    ASSERT(lookup);
    ASSERT_TRUE(pr.trackEmbedPath(*lookup, 1));

    // Need to resolve some fields to detect a collision. NOTE: the path resolver (and dependency
    // graph) don't know anything about variables, so we use the base collection names here.
    testResolve(pr, "lf", "lf", 0, dsLookup);
    ASSERT(lookup->getSubPipeline());
    auto subPipelineIt = lookup->getSubPipeline()->begin();
    ASSERT_NE(subPipelineIt, lookup->getSubPipeline()->end());
    testResolve(pr, "someField", "someField", 1, subPipelineIt->get(), 1);

    // Second path also fine!
    const auto* dsLookup2 = (++it)->get();
    const auto* lookup2 = dynamic_cast<const DocumentSourceLookUp*>(dsLookup2);
    ASSERT(lookup2);
    ASSERT_TRUE(pr.trackEmbedPath(*lookup2, 2));

    // Resolving fields doesn't cause any issues.
    testResolve(pr, "lf", "lf", 0, dsLookup);
    testResolve(pr, "ff", "ff", 2, nullptr, 2);

    // If we were to resolve some more fields (not in the pipeline above!), we can correctly assign
    // each to the right node.
    testResolve(pr, "embed.someOtherField", "someOtherField", 1, nullptr);
    testResolve(pr, "embed.someOtherField.bar", "someOtherField.bar", 1, nullptr);
    testResolve(pr, "embed2.someOtherField", "someOtherField", 2, nullptr);
    testResolve(pr, "embed2.someOtherField2", "someOtherField2", 2, nullptr);

    // However, we will fail to resolve a field that was dropped by the sub-pipeline $project!
    ASSERT_FALSE(pr.resolve("embed.someDroppedField", nullptr));
}

TEST_F(PathResolverTest, HandleMainPipelineProject) {
    auto pipeline =
        makeBasicTestPipeline("lf",
                              "ff",
                              "embed",
                              "lf2",
                              "ff",
                              "embed2",
                              /* prefix */ R"({$project: {lf: 1, embed: 1, lf2: "computed"}},)");
    pipeline::dependency_graph::DependencyGraph dg(pipeline->getSources(),
                                                   mainCollPathAlwaysScalar);
    PathResolver pr(0, dg);

    // No issues embedding our first path.
    auto it = ++(pipeline->getSources().begin());
    const auto* dsLookup = it->get();
    const auto* lookup = dynamic_cast<const DocumentSourceLookUp*>(dsLookup);
    ASSERT(lookup);
    // No issue tracking embed path, even though it was dropped (modified). We embedded after
    // modifying the path, so this effectively cancels out whatever the first modification was, in
    // particular because unlike a projection, this does not do anything other than completely
    // replace the field (no fancy traversal semantics here).
    ASSERT_TRUE(pr.trackEmbedPath(*lookup, 1));

    // Validate that we're able to resolve join predicates in first $lookup.
    testResolve(pr, "lf", "lf", 0, dsLookup);
    testResolve(pr, "ff", "ff", 1, nullptr, 1);
    // Or a path in the embedded field.
    testResolve(pr, "embed.bar", "bar", 1, nullptr);

    // Validate that we can't resolve a path dropped by the $project.
    ASSERT_FALSE(pr.resolve("someOtherPath", nullptr));

    // Tracking second embed path fine.
    const auto* dsLookup2 = (++it)->get();
    const auto* lookup2 = dynamic_cast<const DocumentSourceLookUp*>(dsLookup2);
    ASSERT(lookup2);
    ASSERT_TRUE(pr.trackEmbedPath(*lookup2, 2));

    // BUT we can't resolve the computed local field!
    ASSERT_FALSE(pr.resolve("lf2", nullptr));
}

}  // namespace mongo::join_ordering
