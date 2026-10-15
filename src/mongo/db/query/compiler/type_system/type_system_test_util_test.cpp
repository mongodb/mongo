// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/type_system/type_system_test_util.h"

#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/unittest/unittest.h"

#include <string>

#include <fmt/ranges.h>

namespace mongo::pipeline::type_system {
namespace {

using namespace std::literals::string_view_literals;

TEST(TypeSystemTestUtilTest, ScalarRepresentativesHoldEveryTypeButObjectAndArray) {
    stdx::unordered_set<BSONType> types;
    for (auto&& value : scalarRepresentatives()) {
        types.insert(value.getType());
    }
    ASSERT_EQ(types.size(), 20);
    ASSERT_FALSE(types.contains(BSONType::object));
    ASSERT_FALSE(types.contains(BSONType::array));
}

TEST(TypeSystemTestUtilTest, CorpusHasExpectedSize) {
    ASSERT_EQ(generateDocumentCorpus("x.y.z").size(), 1744);
}

TEST(TypeSystemTestUtilTest, CorpusHoldsExpectedShapes) {
    const auto assertCorpusHolds = [](std::vector<std::string> paths,
                                      const std::vector<std::string>& jsons) {
        auto corpus = generateCombinedCorpus(paths);
        for (const auto& json : jsons) {
            ASSERT_TRUE(corpus.contains(fromjson(json)))
                << fmt::format("{}", paths) << ": " << json;
        }
    };
    assertCorpusHolds({"x"},
                      {"{}",
                       "{x: 1}",
                       "{x: 'str'}",
                       "{x: null}",
                       "{x: {}}",
                       "{x: []}",
                       "{x: [[]]}",
                       "{x: [1]}",
                       "{x: [[1]]}",
                       "{x: [null, 1]}",
                       "{x: 1, x2: 1}"});
    assertCorpusHolds({"x.y"},
                      {"{x: {y: 1}}",
                       "{x: {y: 'str'}}",
                       "{x: [{y: 1}]}",
                       "{x: [[{y: 1}]]}",
                       "{x: [[{y: 'str'}]]}",
                       "{x: 1, x2: 1}",
                       "{x: {y: 1, y2: 1}}",
                       "{x: [{y: 1, y2: 1}]}"});
    assertCorpusHolds({"x.y.z"},
                      {"{x: {y: {}}}",
                       "{x: {y: {z: 1}}}",
                       "{x: {y: {z: {}}}}",
                       "{x: {y: [{z: 1}]}}",
                       "{x: [{y: {z: 1}}]}",
                       "{x: [null, {y: [null, {z: 1}]}]}",
                       "{x: [[{y: [[{z: [[1]]}]]}]]}"});
    assertCorpusHolds({"x.0"},
                      {"{x: [1]}",
                       "{x: {'0': 1}}",
                       "{x: [{'0': 1}]}",
                       "{x: [null, {'0': 1}]}",
                       "{x: {'0': [1]}}"});
    assertCorpusHolds({"x.0.y"},
                      {"{x: {'0': {y: 1}}}",
                       "{x: [{'0': {y: 1}}]}",
                       "{x: {'0': [{y: 1}]}}",
                       "{x: [{'0': [{y: 1}]}]}"});
    assertCorpusHolds({"x."},
                      {"{x: {'': 1}}",
                       "{x: {'': {}}}",
                       "{x: [{'': 1}]}",
                       "{x: {'': [1]}}",
                       "{x: [null, {'': 1}]}"});
    assertCorpusHolds({"x..y"},
                      {"{x: {'': {y: 1}}}",
                       "{x: [{'': {y: 1}}]}",
                       "{x: {'': [{y: 1}]}}",
                       "{x: [{'': [{y: 1}]}]}"});
    assertCorpusHolds(
        {"x.y", "x.z"},
        {"{}", "{x: 1}", "{x: {y: 1}}", "{x: {z: 1}}", "{x: [{y: 1}]}", "{x: [{z: 1}]}"});
}

TEST(TypeSystemTestUtilTest, MatchTraversalCorpusHoldsOnlyItsScalars) {
    auto corpus = generateDocumentCorpus("x", ScalarCorpus::kMatchTraversal);
    ASSERT_TRUE(corpus.contains(fromjson("{}")));
    ASSERT_TRUE(corpus.contains(fromjson("{x: null}")));
    ASSERT_TRUE(corpus.contains(BSON("x" << 1)));
    ASSERT_TRUE(corpus.contains(BSON("x" << 1.0)));
    ASSERT_TRUE(corpus.contains(fromjson("{x: [null, {}]}")));
    ASSERT_FALSE(corpus.contains(fromjson("{x: 'str'}")));
    ASSERT_EQ(corpus.size(), 20);
}

TEST(TypeSystemTestUtilTest, MatchTraversalCorpusWithNumericPath) {
    auto corpus = generateDocumentCorpus("0", ScalarCorpus::kMatchTraversal);
    ASSERT_TRUE(corpus.contains(fromjson("{}")));
    ASSERT_TRUE(corpus.contains(fromjson("{'0': null}")));
    ASSERT_TRUE(corpus.contains(BSON("0" << 1)));
    ASSERT_TRUE(corpus.contains(BSON("0" << 1.0)));
    ASSERT_TRUE(corpus.contains(fromjson("{'0': [null, {}]}")));
    ASSERT_FALSE(corpus.contains(fromjson("{'0': 'str'}")));
    ASSERT_EQ(corpus.size(), 20);
}

TEST(TypeSystemTestUtilTest, CorpusOfPathContainsCorpusOfItsPrefix) {
    const auto assertContainsPrefixCorpus = [](std::string_view path, std::string_view prefix) {
        auto corpus = generateDocumentCorpus(path);
        for (const auto& document : generateDocumentCorpus(prefix)) {
            ASSERT_TRUE(corpus.contains(document)) << path << ": " << document.toString();
        }
    };
    assertContainsPrefixCorpus("x.y", "x");
    assertContainsPrefixCorpus("x.y.z", "x.y");
}

}  // namespace
}  // namespace mongo::pipeline::type_system
