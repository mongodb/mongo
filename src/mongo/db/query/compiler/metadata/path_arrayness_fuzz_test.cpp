// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/initializer.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/query/compiler/metadata/path_arrayness.h"
#include "mongo/db/query/query_fcv_environment_for_test.h"

#include <array>
#include <string>
#include <tuple>
#include <vector>

#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"

namespace mongo {
namespace {

// FuzzTest's gtest main doesn't run MongoDB's MONGO_INITIALIZERs (e.g. OIDGeneration) and the
// FCV listener installed by unittest_main_core. Register a gtest Environment so SetUp runs once
// before any test body, before ExpressionContextForTest construction acquires an FCV snapshot or
// creates a ServiceContext.
class MongoInfrastructureEnv : public ::testing::Environment {
public:
    void SetUp() override {
        runGlobalInitializersOrDie({});
        QueryFCVEnvironmentForTest::setUp();
    }
};

[[maybe_unused]] const auto kRegisterMongoInfraEnv =
    ::testing::AddGlobalTestEnvironment(new MongoInfrastructureEnv);

// Each PathOp encodes one addPath() call:
//   components  — indices into kNames, joined with '.' to form the FieldPath
//   arrayDepths — depths that are multikey; values >= components.size() are ignored
//   isFullRebuild
//
// int is used instead of uint8_t to avoid fuzztest's byte-blob serialization path, which
// crashes with negative-size-param when Centipede passes raw byte sequences to the domain
// parser.
using PathOp = std::tuple<std::vector<int>, std::vector<int>, bool>;

static constexpr std::array<const char*, 4> kNames = {"a", "b", "c", "d"};

FieldPath buildFieldPath(const std::vector<int>& comps) {
    std::string s;
    for (size_t i = 0; i < comps.size(); ++i) {
        if (i > 0)
            s += '.';
        s += kNames[comps[i] % kNames.size()];
    }
    return FieldPath(s);
}

PathArrayness buildPathArrayness(const std::vector<PathOp>& ops) {
    PathArrayness pa;
    for (const auto& [comps, depths, isFullRebuild] : ops) {
        MultikeyComponents mc;
        for (int d : depths)
            if (d >= 0 && static_cast<size_t>(d) < comps.size())
                mc.insert(static_cast<MultikeyComponents::value_type>(d));
        pa.addPath(buildFieldPath(comps), mc, isFullRebuild);
    }
    return pa;
}

auto PathOpsDomain() {
    return fuzztest::VectorOf(
               fuzztest::TupleOf(
                   fuzztest::VectorOf(fuzztest::InRange(0, 3)).WithMinSize(1).WithMaxSize(5),
                   fuzztest::VectorOf(fuzztest::InRange(0, 4)).WithMaxSize(5),
                   fuzztest::Arbitrary<bool>()))
        .WithMaxSize(20);
}

auto QueryCompsDomain() {
    return fuzztest::VectorOf(
               fuzztest::VectorOf(fuzztest::InRange(0, 3)).WithMinSize(1).WithMaxSize(5))
        .WithMaxSize(10);
}

void EmptyOldTrieNeverInvalidates(std::vector<PathOp> ops) {
    ExpressionContextForTest expCtx;
    PathArrayness current = buildPathArrayness(ops);
    EXPECT_FALSE(PathArrayness::getFirstInvalidatedPath(
                     expCtx.nonArrayPathsForNss(expCtx.getNamespaceString()), current)
                     .has_value());
}

FUZZ_TEST(PathArraynessFuzz, EmptyOldTrieNeverInvalidates).WithDomains(PathOpsDomain());

void HasInvalidatedPathsMatchesManualCheck(std::vector<PathOp> oldOps,
                                           std::vector<std::vector<int>> queryComps,
                                           std::vector<PathOp> currentOps) {
    auto old = std::make_shared<PathArrayness>(buildPathArrayness(oldOps));

    ExpressionContextForTest expCtx;
    expCtx.setPathArraynessForNss(expCtx.getNamespaceString(), old);
    const auto& nss = expCtx.getNamespaceString();
    for (const auto& comps : queryComps)
        expCtx.canPathBeArrayForNss(buildFieldPath(comps), nss);

    PathArrayness current = buildPathArrayness(currentOps);

    ExpressionContextForTest checkExpCtx;
    checkExpCtx.setPathArraynessForNss(checkExpCtx.getNamespaceString(),
                                       std::make_shared<PathArrayness>(current));
    const auto& checkNss = checkExpCtx.getNamespaceString();
    bool expected = false;
    for (const auto& path : expCtx.nonArrayPathsForNss(nss))
        if (checkExpCtx.canPathBeArrayForNss(path, checkNss))
            expected = true;

    EXPECT_EQ(PathArrayness::getFirstInvalidatedPath(expCtx.nonArrayPathsForNss(nss), current)
                  .has_value(),
              expected);
}

FUZZ_TEST(PathArraynessFuzz, HasInvalidatedPathsMatchesManualCheck)
    .WithDomains(PathOpsDomain(), QueryCompsDomain(), PathOpsDomain());

void SelfDoesNotInvalidate(std::vector<PathOp> ops, std::vector<std::vector<int>> queryComps) {
    auto old = std::make_shared<PathArrayness>(buildPathArrayness(ops));
    ExpressionContextForTest expCtx;
    expCtx.setPathArraynessForNss(expCtx.getNamespaceString(), old);
    const auto& nss = expCtx.getNamespaceString();
    for (const auto& comps : queryComps)
        expCtx.canPathBeArrayForNss(buildFieldPath(comps), nss);
    EXPECT_FALSE(
        PathArrayness::getFirstInvalidatedPath(expCtx.nonArrayPathsForNss(nss), *old).has_value());
}

FUZZ_TEST(PathArraynessFuzz, SelfDoesNotInvalidate)
    .WithDomains(PathOpsDomain(), QueryCompsDomain());

}  // namespace
}  // namespace mongo
