/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/pipeline/pipeline_metadata_tree.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/pipeline/document_source_bucket_auto.h"
#include "mongo/db/pipeline/document_source_facet.h"
#include "mongo/db/pipeline/document_source_graph_lookup.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/document_source_tee_consumer.h"
#include "mongo/db/pipeline/document_source_unwind.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <numeric>
#include <stack>
#include <string>
#include <typeinfo>
#include <vector>

#include <absl/container/node_hash_map.h>
#include <boost/exception/exception.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

class PipelineMetadataTreeTest : public AggregationContextFixture {
protected:
    PipelineMetadataTreeTest() {
        ShardingState::create(getServiceContext());
    }

    auto jsonToPipeline(StringData jsonArray) {
        const auto inputBson = fromjson("{pipeline: " + jsonArray + "}");

        ASSERT_EQUALS(inputBson["pipeline"].type(), BSONType::array);
        auto rawPipeline = parsePipelineFromBSON(inputBson["pipeline"]);
        NamespaceString testNss =
            NamespaceString::createNamespaceString_forTest("test", "collection");
        AggregateCommandRequest request(testNss, rawPipeline);
        getExpCtx()->setNamespaceString(testNss);

        return Pipeline::parse(request.getPipeline(), getExpCtx());
    }

    template <typename T, typename... Args>
    std::vector<T> makeVector(Args&&... args) {
        std::vector<T> v;
        v.reserve(sizeof...(Args));
        (v.push_back(std::forward<Args>(args)), ...);
        return v;
    }

    void introduceCollection(StringData collectionName) {
        NamespaceString fromNs =
            NamespaceString::createNamespaceString_forTest("test", collectionName);
        _resolvedNamespaces.insert({fromNs, {fromNs, std::vector<BSONObj>()}});
        getExpCtx()->setResolvedNamespaces(_resolvedNamespaces);
    }

private:
    ResolvedNamespaceMap _resolvedNamespaces;
};

using namespace pipeline_metadata_tree;

/**
 * A test data structure to hold pipeline metadata.
 */
template <typename T>
struct TestThingGeneric {
    bool operator==(const TestThingGeneric& other) const {
        return value == other.value;
    }
    T value;
};

/**
 * Define the stream output operator to enable the test framework to print out data in case of
 * test assertion failures.
 */
template <typename T>
std::ostream& operator<<(std::ostream& os, const TestThingGeneric<T>& tt) {
    os << tt.value;
    return os;
}

/**
 * Define the stream output operator to enable the test framework to print out data in case of
 * test assertion failures.
 */
template <typename T>
std::ostream& operator<<(std::ostream& os, const Stage<T>& stage) {
    os << "(" << stage.contents << " ";
    if (stage.principalChild) {
        os << *stage.principalChild;
    } else {
        os << "()";
    }
    os << " [";
    for (const auto& additionalChild : stage.additionalChildren) {
        os << additionalChild << " ";
    }
    os << "])";
    return os;
}

/**
 * A factory function to create Pipeline Metadata's Stage with test metadata.
 */
template <typename T>
Stage<TestThingGeneric<T>> makeStage(
    T&& contents,
    std::unique_ptr<Stage<TestThingGeneric<T>>> principalChild = {},
    std::vector<Stage<TestThingGeneric<T>>>&& additionalChildren = {}) {
    return Stage<TestThingGeneric<T>>(TestThingGeneric<T>{std::move(contents)},
                                      std::move(principalChild),
                                      std::move(additionalChildren));
}

/**
 * A factory function to create a unique_ptr of Pipeline Metadata's Stage with test metadata.
 */
template <typename T>
std::unique_ptr<Stage<TestThingGeneric<T>>> makeUniqueStage(
    T&& contents,
    std::unique_ptr<Stage<TestThingGeneric<T>>> principalChild = {},
    std::vector<Stage<TestThingGeneric<T>>>&& additionalChildren = {}) {
    return std::make_unique<Stage<TestThingGeneric<T>>>(TestThingGeneric<T>{std::move(contents)},
                                                        std::move(principalChild),
                                                        std::move(additionalChildren));
}

/**
 * Overloaded factory method that allow to use raw char* to initialize std::string test metadata.
 */
Stage<TestThingGeneric<std::string>> makeStage(
    const char* contents,
    std::unique_ptr<Stage<TestThingGeneric<std::string>>> principalChild = {},
    std::vector<Stage<TestThingGeneric<std::string>>>&& additionalChildren = {}) {
    return makeStage(
        std::string(contents), std::move(principalChild), std::move(additionalChildren));
}

/**
 * Overloaded factory method that allow to use raw char* to initialize std::string test metadata.
 */
std::unique_ptr<Stage<TestThingGeneric<std::string>>> makeUniqueStage(
    const char* contents,
    std::unique_ptr<Stage<TestThingGeneric<std::string>>> principalChild = {},
    std::vector<Stage<TestThingGeneric<std::string>>>&& additionalChildren = {}) {
    return makeUniqueStage(
        std::string(contents), std::move(principalChild), std::move(additionalChildren));
}

/**
 * Builds a string representation of stages leading up to the current stage. This is done by
 * concatenating a character representing the current stage to the string from the previous stage.
 * In addition, lookup and facet append a string containing each of the off-the-end strings from
 * their sub-pipelines.
 */
TestThingGeneric<std::string> buildRepresentativeString(
    const TestThingGeneric<std::string>& previousThing,
    const std::vector<TestThingGeneric<std::string>>& extraThings,
    const DocumentSource& source) {
    if (typeid(source) == typeid(DocumentSourceMatch))
        return {previousThing.value + "m"};
    if (typeid(source) == typeid(DocumentSourceSingleDocumentTransformation))
        return {previousThing.value + "p"};
    if (typeid(source) == typeid(DocumentSourceGraphLookUp))
        return {previousThing.value + "x"};
    if (typeid(source) == typeid(DocumentSourceUnwind))
        return {previousThing.value + "u"};
    if (typeid(source) == typeid(DocumentSourceGroup))
        return {previousThing.value + "g"};
    if (auto lookupSource = dynamic_cast<const DocumentSourceLookUp*>(&source)) {
        if (lookupSource->hasPipeline())
            return {previousThing.value + "l[" + extraThings.front().value + "]"};
        else
            return {previousThing.value + "l"};
    }
    if (typeid(source) == typeid(DocumentSourceFacet))
        return {previousThing.value + "f[" +
                std::accumulate(std::next(extraThings.begin()),
                                extraThings.end(),
                                extraThings.front().value,
                                [](auto l, auto r) { return l + ", " + r.value; }) +
                "]"};
    if (auto unionWithSource = dynamic_cast<const DocumentSourceUnionWith*>(&source)) {
        if (unionWithSource->hasNonEmptyPipeline()) {
            return {previousThing.value + "w[" + extraThings.front().value + "]"};
        } else {
            return {previousThing.value + "w"};
        }
    }
    if (typeid(source) == typeid(DocumentSourceTeeConsumer))
        return {previousThing.value + "t"};
    if (typeid(source) == typeid(DocumentSourceSort))
        return {previousThing.value + "s"};
    if (typeid(source) == typeid(DocumentSourceBucketAuto))
        return {previousThing.value + "b"};
    if (typeid(source) == typeid(DocumentSourceLimit))
        return {previousThing.value + "#"};
    return {previousThing.value + "?"};
}

TEST_F(PipelineMetadataTreeTest, LinearPipelinesConstructProperTrees) {
    using TestThing = TestThingGeneric<int>;

    TestThing initial{23};
    auto ignoreDocumentSourceAddOne =
        [](const auto& previousThing, const auto&, const auto&) -> TestThing {
        return {previousThing.value + 1};
    };

    ASSERT_EQ(
        [&]() {
            auto pipePtr = jsonToPipeline("[{$project: {name: 1}}]");
            return makeTree<TestThing>(
                {{NamespaceString::createNamespaceString_forTest("test.collection"), initial}},
                *pipePtr,
                ignoreDocumentSourceAddOne);
        }()
            .first.value(),
        makeStage(23));

    ASSERT_EQ(
        [&]() {
            auto pipePtr = jsonToPipeline(
                "[{$project: {name: 1, status: 1}}, "
                "{$match: {status: \"completed\"}}]");
            return makeTree<TestThing>(
                {{NamespaceString::createNamespaceString_forTest("test.collection"), initial}},
                *pipePtr,
                ignoreDocumentSourceAddOne);
        }()
            .first.value(),
        makeStage(24, makeUniqueStage(23)));

    ASSERT_EQ(
        [&]() {
            auto pipePtr = jsonToPipeline(
                "[{$project: {name: 1, status: 1}}, "
                "{$match: {status: \"completed\"}}, "
                "{$match: {status: \"completed\"}}, "
                "{$match: {status: \"completed\"}}, "
                "{$match: {status: \"completed\"}}, "
                "{$match: {status: \"completed\"}}]");
            return makeTree<TestThing>(
                {{NamespaceString::createNamespaceString_forTest("test.collection"), initial}},
                *pipePtr,
                ignoreDocumentSourceAddOne);
        }()
            .first.value(),
        makeStage(28,
                  makeUniqueStage(
                      27,
                      makeUniqueStage(
                          26, makeUniqueStage(25, makeUniqueStage(24, makeUniqueStage(23)))))));
}

TEST_F(PipelineMetadataTreeTest, BranchingPipelinesConstructProperTrees) {
    using TestThing = TestThingGeneric<std::string>;

    introduceCollection("folios");
    introduceCollection("trades");
    introduceCollection("instruments");

    ASSERT_EQ(
        [&]() {
            auto pipePtr = jsonToPipeline(
                "[{$match: {ident: {$in: [12345]}}}, "
                "{$project: {_id: 0, ident: 1}}, "
                "{$graphLookup: {from: \"folios\", startWith: 12345, connectFromField: \"ident\", "
                "connectToField: \"mgr\", as: \"sub_positions\", maxDepth: 100}}, "
                "{$unwind: \"$sub_positions\"}, "
                "{$lookup: {from: \"trades\", as: \"trade\", let: {sp: \"sub_positions.ident\"}, "
                "pipeline: [{$match: {$expr: {$eq: [\"$$sp\", \"$opcvm\"]}}}]}}, "
                "{$unwind: \"$trade\"}, "
                "{$lookup: {from: \"instruments\", as: \"instr\", localField: \"trade.sicovam\", "
                "foreignField: \"sicovam\"}}, "
                "{$unwind: \"$instr\"}, "
                "{$group: {_id: {PositionID: \"$trade.mvtident\", \"InstrumentReference\": "
                "\"$instr.libelle\"}, NumberOfSecurities: {$sum:\"$trade.quantite\"}}}]");
            return makeTree<TestThing>(
                {{NamespaceString::createNamespaceString_forTest("test.collection"), {"1"}},
                 {NamespaceString::createNamespaceString_forTest("test.folios"), {"2"}},
                 {NamespaceString::createNamespaceString_forTest("test.trades"), {"2"}},
                 {NamespaceString::createNamespaceString_forTest("test.instruments"), {"2"}}},
                *pipePtr,
                buildRepresentativeString);
        }()
            .first.value(),
        Stage(TestThing{"1mpxul[2m]ulu"},
              makeUniqueStage(
                  "1mpxul[2m]ul",
                  makeUniqueStage(
                      "1mpxul[2m]u",
                      makeUniqueStage(
                          "1mpxul[2m]",
                          makeUniqueStage(
                              "1mpxu",
                              makeUniqueStage(
                                  "1mpx",
                                  makeUniqueStage(
                                      "1mp",
                                      makeUniqueStage("1m", makeUniqueStage("1", {}, {}), {}),
                                      {}),
                                  {}),
                              makeVector<Stage<TestThing>>(Stage(TestThing{"2"}, {}, {}))),
                          {}),
                      makeVector<Stage<TestThing>>(Stage(TestThing{"2"}, {}, {}))),
                  {}),
              {}));

    ASSERT_EQ(
        [&]() {
            auto pipePtr = jsonToPipeline(
                "[{$facet:{"
                "categorizedByTags: "
                "[{$unwind: \"$tags\"}, {$sortByCount: \"$tags\"}], "
                "categorizedByYears: [{$match: { year: {$exists: 1}}}, "
                "{$bucket: {groupBy: \"$year\", boundaries: [ 2000, 2010, 2015, 2020]}}], "
                "\"categorizedByYears(Auto)\": [{$bucketAuto: {groupBy: \"$year\", buckets: "
                "2}}]}}, "
                "{$limit: 12}]");
            return makeTree<TestThing>(
                {{NamespaceString::createNamespaceString_forTest("test.collection"), {""}}},
                *pipePtr,
                buildRepresentativeString);
        }()
            .first.value(),
        Stage(TestThing{"f[tugs, tmgs, tb]"},
              makeUniqueStage(
                  "",
                  {},
                  makeVector<Stage<TestThing>>(
                      Stage(TestThing{"tug"},
                            makeUniqueStage(
                                "tu", makeUniqueStage("t", makeUniqueStage("", {}, {}), {}), {}),
                            {}),
                      Stage(TestThing{"tmg"},
                            makeUniqueStage(
                                "tm", makeUniqueStage("t", makeUniqueStage("", {}, {}), {}), {}),
                            {}),
                      Stage(TestThing{"t"}, makeUniqueStage("", {}, {}), {}))),
              {}));
}

TEST_F(PipelineMetadataTreeTest, BranchingPipelinesConstructProperTreesForUnionWith) {
    using TestThing = TestThingGeneric<std::string>;

    introduceCollection("Pluto");
    introduceCollection("Ceres");
    introduceCollection("Sedna");

    auto pipePtr = jsonToPipeline(
        R"(
            [
                { $match: {ident: 11} },
                { $unionWith: { coll: "Pluto", pipeline: [{ $match: {ident: 12} }] } },
                { $unionWith: { coll: "Ceres"} },
                { $unionWith: { coll: "Sedna", pipeline: [{ $match: {ident: 13} }] } },
                { $limit: 14 }
            ]
        )");
    auto result = makeTree<TestThing>(
        {
            {NamespaceString::createNamespaceString_forTest("test.collection"), {"1"}},
            {NamespaceString::createNamespaceString_forTest("test.Pluto"), {"2"}},
            {NamespaceString::createNamespaceString_forTest("test.Ceres"), {"3"}},
            {NamespaceString::createNamespaceString_forTest("test.Sedna"), {"4"}},
        },
        *pipePtr,
        buildRepresentativeString);

    auto stage0 = makeUniqueStage("1");              // Scanning #1: collection
    auto stage1 = makeUniqueStage("1m");             // $match
    auto stage2 = makeUniqueStage("1mw[2m]");        // $unioWith from #2: Pluto
    auto stage3 = makeUniqueStage("1mw[2m]w");       // $unioWith from #3: Ceres
    auto stage4 = makeUniqueStage("1mw[2m]ww[4m]");  // $unioWith from #4: Sedna

    stage1->additionalChildren.emplace_back(makeStage("2"));  // #2: Pluto
    // #3: Ceres is missing here because its $unionWith does not contains a sub-pipeline.
    stage3->additionalChildren.emplace_back(makeStage("4"));  // #4: Sedna

    stage1->principalChild = std::move(stage0);
    stage2->principalChild = std::move(stage1);
    stage3->principalChild = std::move(stage2);
    stage4->principalChild = std::move(stage3);

    ASSERT_EQ(result.first.value(), *stage4);
    // This '#' at the end means $limit stage.
    ASSERT_EQ(result.second.value, std::string("1mw[2m]ww[4m]#"));
}

TEST_F(PipelineMetadataTreeTest, ZipWalksAPipelineAndTreeInTandemAndInOrder) {
    struct TestThing {
        auto operator==(const TestThing& other) const {
            return typeInfo == other.typeInfo;
        }
        const std::type_info* typeInfo = nullptr;
    };

    auto takeTypeInfo = [](const auto&, const auto&, const DocumentSource& source) -> TestThing {
        return {&typeid(source)};
    };

    // The stack holds one element for each branch of the tree.
    std::stack<const std::type_info*> previousStack;
    // Verifies that we walk each branch from leaf upwards towards the root when invoking the
    // zip() function, since we will throw if the top of the stack (which is the branch being
    // actively walked) has a typeid which does not match the typeid of the previous stage.
    auto tookTypeInfoOrThrow = [&previousStack](auto* stage, auto* source) {
        for ([[maybe_unused]] auto&& child : stage->additionalChildren) {
            // If we have a lookup source without a pipeline, we should not pop from the previous
            // stack. This is because the additional child for the lookup case is actually an NoOp
            // stage, since we don't have a pipeline stage that generates the output.
            if (auto* lookupSource = dynamic_cast<DocumentSourceLookUp*>(source); lookupSource &&
                lookupSource->hasLocalFieldForeignFieldJoin() && !lookupSource->hasPipeline()) {
                continue;
            }
            previousStack.pop();
        }
        if (!stage->principalChild)
            previousStack.push(nullptr);
        if (auto typeInfo = stage->contents.typeInfo;
            (previousStack.top() && typeInfo && *previousStack.top() != *typeInfo) ||
            (previousStack.top() && !typeInfo) || (!previousStack.top() && typeInfo))
            uasserted(51163, "Walk did not proceed in expected order!");
        previousStack.top() = &typeid(*source);
    };

    introduceCollection("folios");
    introduceCollection("trades");
    introduceCollection("instruments");

    ASSERT_DOES_NOT_THROW([&]() {
        auto pipePtr = jsonToPipeline(
            "[{$match: {ident: {$in: [12345]}}}, "
            "{$project: {_id: 0, ident: 1}}, "
            "{$graphLookup: {from: \"folios\", startWith: 12345, connectFromField: \"ident\", "
            "connectToField: \"mgr\", as: \"sub_positions\", maxDepth: 100}}, "
            "{$unwind: \"$sub_positions\"}, "
            "{$lookup: {from: \"trades\", as: \"trade\", let: {sp: \"sub_positions.ident\"}, "
            "pipeline: [{$match: {$expr: {$eq: [\"$$sp\", \"$opcvm\"]}}}]}}, "
            "{$unwind: \"$trade\"}, "
            "{$lookup: {from: \"instruments\", as: \"instr\", localField: \"trade.sicovam\", "
            "foreignField: \"sicovam\"}}, "
            "{$unwind: \"$instr\"}, "
            "{$group: {_id: {PositionID: \"$trade.mvtident\", \"InstrumentReference\": "
            "\"$instr.libelle\"}, NumberOfSecurities: {$sum:\"$trade.quantite\"}}}]");
        auto tree = makeTree<TestThing>(
                        {{NamespaceString::createNamespaceString_forTest("test.collection"), {}},
                         {NamespaceString::createNamespaceString_forTest("test.folios"), {}},
                         {NamespaceString::createNamespaceString_forTest("test.trades"), {}},
                         {NamespaceString::createNamespaceString_forTest("test.instruments"), {}}},
                        *pipePtr,
                        takeTypeInfo)
                        .first;
        zip<TestThing>(&tree.value(), &*pipePtr, tookTypeInfoOrThrow);
        previousStack.pop();
    }());

    ASSERT_DOES_NOT_THROW([&]() {
        auto pipePtr = jsonToPipeline(
            "[{$facet:{"
            "categorizedByTags: "
            "[{$unwind: \"$tags\"}, {$sortByCount: \"$tags\"}], "
            "categorizedByYears: [{$match: { year: {$exists: 1}}}, "
            "{$bucket: {groupBy: \"$year\", boundaries: [ 2000, 2010, 2015, 2020]}}], "
            "\"categorizedByYears(Auto)\": [{$bucketAuto: {groupBy: \"$year\", buckets: "
            "2}}]}}, "
            "{$limit: 12}]");
        auto tree = makeTree<TestThing>(
                        {{NamespaceString::createNamespaceString_forTest("test.collection"), {}},
                         {NamespaceString::createNamespaceString_forTest("test.collection"), {}}},
                        *pipePtr,
                        takeTypeInfo)
                        .first;
        zip<TestThing>(&tree.value(), &*pipePtr, tookTypeInfoOrThrow);
        previousStack.pop();
    }());
}

TEST_F(PipelineMetadataTreeTest, MakeTreeWithEmptyPipeline) {
    auto pipeline = Pipeline::parse({}, getExpCtx());
    auto result = makeTree<std::string>({{getExpCtx()->getNamespaceString(), std::string("input")}},
                                        *pipeline,
                                        [](const auto&, const auto&, const DocumentSource& source) {
                                            return std::string("not called");
                                        });
    ASSERT_FALSE(result.first);
    ASSERT_EQ(result.second, "input"_sd);
}

TEST_F(PipelineMetadataTreeTest, BranchingPipelineMissesInitialStageContents) {
    introduceCollection("trades");
    auto pipeline = jsonToPipeline(
        "[{$lookup: {from: \"trades\", as: \"trade\", let: {sp: \"sub_positions.ident\"}, "
        "pipeline: [{$match: {$expr: {$eq: [\"$$sp\", \"$opcvm\"]}}}]}}]");
    ASSERT_THROWS_CODE(
        makeTree<std::string>({{NamespaceString::createNamespaceString_forTest("test.collection"),
                                std::string("input")}},
                              *pipeline,
                              [](const auto&, const auto&, const DocumentSource& source) {
                                  return std::string("not called");
                              }),
        AssertionException,
        51213);
}

}  // namespace
}  // namespace mongo
