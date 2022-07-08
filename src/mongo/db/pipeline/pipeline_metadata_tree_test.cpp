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

#include "mongo/platform/basic.h"

#include <functional>
#include <memory>
#include <numeric>
#include <stack>
#include <string>
#include <typeinfo>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/bson/json.h"
#include "mongo/db/namespace_string.h"
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
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/pipeline_metadata_tree.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class PipelineMetadataTreeTest : public AggregationContextFixture {
protected:
    auto jsonToPipeline(StringData jsonArray) {
        const auto inputBson = fromjson("{pipeline: " + jsonArray + "}");

        ASSERT_EQUALS(inputBson["pipeline"].type(), BSONType::Array);
        auto rawPipeline = parsePipelineFromBSON(inputBson["pipeline"]);
        NamespaceString testNss("test", "collection");
        AggregateCommandRequest request(testNss, rawPipeline);
        getExpCtx()->ns = testNss;

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
        NamespaceString fromNs("test", collectionName);
        _resolvedNamespaces.insert({fromNs.coll().toString(), {fromNs, std::vector<BSONObj>()}});
        getExpCtx()->setResolvedNamespaces(_resolvedNamespaces);
    }

private:
    StringMap<ExpressionContext::ResolvedNamespace> _resolvedNamespaces;
};

using namespace pipeline_metadata_tree;

TEST_F(PipelineMetadataTreeTest, LinearPipelinesConstructProperTrees) {
    struct TestThing {
        auto operator==(const TestThing& other) const {
            return number == other.number;
        }
        int number;
    } initial{23};
    auto ignoreDocumentSourceAddOne =
        [](const auto& previousThing, const auto&, const auto&) -> TestThing {
        return {previousThing.number + 1};
    };

    auto makeUniqueStage = [&](auto&& contents,
                               std::unique_ptr<Stage<TestThing>> principalChild,
                               std::vector<Stage<TestThing>>&& additionalChildren) {
        return std::make_unique<Stage<TestThing>>(
            std::move(contents), std::move(principalChild), std::move(additionalChildren));
    };

    ASSERT([&]() {
        auto pipePtr = jsonToPipeline("[{$project: {name: 1}}]");
        return makeTree<TestThing>(
            {{NamespaceString("test.collection"), initial}}, *pipePtr, ignoreDocumentSourceAddOne);
    }()
               .first.get() == Stage(TestThing{23}, {}, {}));

    ASSERT([&]() {
        auto pipePtr = jsonToPipeline(
            "[{$project: {name: 1, status: 1}}, "
            "{$match: {status: \"completed\"}}]");
        return makeTree<TestThing>(
            {{NamespaceString("test.collection"), initial}}, *pipePtr, ignoreDocumentSourceAddOne);
    }()
               .first.get() == Stage(TestThing{24}, makeUniqueStage(TestThing{23}, {}, {}), {}));

    ASSERT([&]() {
        auto pipePtr = jsonToPipeline(
            "[{$project: {name: 1, status: 1}}, "
            "{$match: {status: \"completed\"}}, "
            "{$match: {status: \"completed\"}}, "
            "{$match: {status: \"completed\"}}, "
            "{$match: {status: \"completed\"}}, "
            "{$match: {status: \"completed\"}}]");
        return makeTree<TestThing>(
            {{NamespaceString("test.collection"), initial}}, *pipePtr, ignoreDocumentSourceAddOne);
    }()
               .first.get() ==
           Stage(TestThing{28},
                 makeUniqueStage(
                     TestThing{27},
                     makeUniqueStage(
                         TestThing{26},
                         makeUniqueStage(TestThing{25},
                                         makeUniqueStage(TestThing{24},
                                                         makeUniqueStage(TestThing{23}, {}, {}),
                                                         {}),
                                         {}),
                         {}),
                     {}),
                 {}));
}


TEST_F(PipelineMetadataTreeTest, BranchingPipelinesConstructProperTrees) {
    struct TestThing {
        auto operator==(const TestThing& other) const {
            return string == other.string;
        }
        std::string string;
    };

    auto makeUniqueStage = [&](auto&& contents,
                               std::unique_ptr<Stage<TestThing>> principalChild,
                               std::vector<Stage<TestThing>>&& additionalChildren) {
        return std::make_unique<Stage<TestThing>>(
            std::move(contents), std::move(principalChild), std::move(additionalChildren));
    };

    // Builds a string representation of stages leading up to the current stage. This is done by
    // concatenating a character representing the current stage to the string from the previous
    // stage. In addition, lookup and facet append a string containing each of the off-the-end
    // strings from their sub-pipelines.
    auto buildRepresentativeString = [](const auto& previousThing,
                                        const auto& extraThings,
                                        const DocumentSource& source) -> TestThing {
        if (typeid(source) == typeid(DocumentSourceMatch))
            return {previousThing.string + "m"};
        if (typeid(source) == typeid(DocumentSourceSingleDocumentTransformation))
            return {previousThing.string + "p"};
        if (typeid(source) == typeid(DocumentSourceGraphLookUp))
            return {previousThing.string + "x"};
        if (typeid(source) == typeid(DocumentSourceUnwind))
            return {previousThing.string + "u"};
        if (typeid(source) == typeid(DocumentSourceGroup))
            return {previousThing.string + "g"};
        if (auto lookupSource = dynamic_cast<const DocumentSourceLookUp*>(&source)) {
            if (lookupSource->hasPipeline())
                return {previousThing.string + "l[" + extraThings.front().string + "]"};
            else
                return {previousThing.string + "l"};
        }
        if (typeid(source) == typeid(DocumentSourceFacet))
            return {previousThing.string + "f[" +
                    std::accumulate(std::next(extraThings.begin()),
                                    extraThings.end(),
                                    extraThings.front().string,
                                    [](auto l, auto r) { return l + ", " + r.string; }) +
                    "]"};
        if (typeid(source) == typeid(DocumentSourceTeeConsumer))
            return {previousThing.string + "t"};
        if (typeid(source) == typeid(DocumentSourceSort))
            return {previousThing.string + "s"};
        if (typeid(source) == typeid(DocumentSourceBucketAuto))
            return {previousThing.string + "b"};
        if (typeid(source) == typeid(DocumentSourceLimit))
            return {previousThing.string + "#"};
        return {previousThing.string + "?"};
    };

    introduceCollection("folios");
    introduceCollection("trades");
    introduceCollection("instruments");

    ASSERT([&]() {
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
        return makeTree<TestThing>({{NamespaceString("test.collection"), {"1"}},
                                    {NamespaceString("test.folios"), {"2"}},
                                    {NamespaceString("test.trades"), {"2"}},
                                    {NamespaceString("test.instruments"), {"2"}}},
                                   *pipePtr,
                                   buildRepresentativeString);
    }()
               .first.get() ==
           Stage(TestThing{"1mpxul[2m]ulu"},
                 makeUniqueStage(
                     TestThing{"1mpxul[2m]ul"},
                     makeUniqueStage(
                         TestThing{"1mpxul[2m]u"},
                         makeUniqueStage(
                             TestThing{"1mpxul[2m]"},
                             makeUniqueStage(
                                 TestThing{"1mpxu"},
                                 makeUniqueStage(
                                     TestThing{"1mpx"},
                                     makeUniqueStage(
                                         TestThing{"1mp"},
                                         makeUniqueStage(TestThing{"1m"},
                                                         makeUniqueStage(TestThing{"1"}, {}, {}),
                                                         {}),
                                         {}),
                                     {}),
                                 makeVector<Stage<TestThing>>(Stage(TestThing{"2"}, {}, {}))),
                             {}),
                         {}),
                     {}),
                 {}));

    ASSERT([&]() {
        auto pipePtr = jsonToPipeline(
            "[{$facet:{"
            "categorizedByTags: "
            "[{$unwind: \"$tags\"}, {$sortByCount: \"$tags\"}], "
            "categorizedByYears: [{$match: { year: {$exists: 1}}}, "
            "{$bucket: {groupBy: \"$year\", boundaries: [ 2000, 2010, 2015, 2020]}}], "
            "\"categorizedByYears(Auto)\": [{$bucketAuto: {groupBy: \"$year\", buckets: 2}}]}}, "
            "{$limit: 12}]");
        return makeTree<TestThing>(
            {{NamespaceString("test.collection"), {""}}}, *pipePtr, buildRepresentativeString);
    }()
               .first.get() ==
           Stage(TestThing{"f[tugs, tmgs, tb]"},
                 makeUniqueStage(
                     TestThing{""},
                     {},
                     makeVector<Stage<TestThing>>(
                         Stage(TestThing{"tug"},
                               makeUniqueStage(
                                   TestThing{"tu"},
                                   makeUniqueStage(
                                       TestThing{"t"}, makeUniqueStage(TestThing{""}, {}, {}), {}),
                                   {}),
                               {}),
                         Stage(TestThing{"tmg"},
                               makeUniqueStage(
                                   TestThing{"tm"},
                                   makeUniqueStage(
                                       TestThing{"t"}, makeUniqueStage(TestThing{""}, {}, {}), {}),
                                   {}),
                               {}),
                         Stage(TestThing{"t"}, makeUniqueStage(TestThing{""}, {}, {}), {}))),
                 {}));
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
    // Verifies that we walk each branch from leaf upwards towards the root when invoking the zip()
    // function, since we will throw if the top of the stack (which is the branch being actively
    // walked) has a typeid which does not match the typeid of the previous stage.
    auto tookTypeInfoOrThrow = [&previousStack](auto* stage, auto* source) {
        for ([[maybe_unused]] auto&& child : stage->additionalChildren)
            previousStack.pop();
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
        auto tree = makeTree<TestThing>({{NamespaceString("test.collection"), {}},
                                         {NamespaceString("test.folios"), {}},
                                         {NamespaceString("test.trades"), {}},
                                         {NamespaceString("test.instruments"), {}}},
                                        *pipePtr,
                                        takeTypeInfo)
                        .first;
        zip<TestThing>(&tree.get(), &*pipePtr, tookTypeInfoOrThrow);
        previousStack.pop();
    }());

    ASSERT_DOES_NOT_THROW([&]() {
        auto pipePtr = jsonToPipeline(
            "[{$facet:{"
            "categorizedByTags: "
            "[{$unwind: \"$tags\"}, {$sortByCount: \"$tags\"}], "
            "categorizedByYears: [{$match: { year: {$exists: 1}}}, "
            "{$bucket: {groupBy: \"$year\", boundaries: [ 2000, 2010, 2015, 2020]}}], "
            "\"categorizedByYears(Auto)\": [{$bucketAuto: {groupBy: \"$year\", buckets: 2}}]}}, "
            "{$limit: 12}]");
        auto tree = makeTree<TestThing>({{NamespaceString("test.collection"), {}},
                                         {NamespaceString("test.collection"), {}}},
                                        *pipePtr,
                                        takeTypeInfo)
                        .first;
        zip<TestThing>(&tree.get(), &*pipePtr, tookTypeInfoOrThrow);
        previousStack.pop();
    }());
}

TEST_F(PipelineMetadataTreeTest, MakeTreeWithEmptyPipeline) {
    auto pipeline = Pipeline::parse({}, getExpCtx());
    auto result =
        makeTree<std::string>({{NamespaceString("unittests.pipeline_test"), std::string("input")}},
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
        makeTree<std::string>({{NamespaceString("test.collection"), std::string("input")}},
                              *pipeline,
                              [](const auto&, const auto&, const DocumentSource& source) {
                                  return std::string("not called");
                              }),
        AssertionException,
        51213);
}

}  // namespace
}  // namespace mongo
