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

#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/query/compiler/metadata/path_arrayness.h"

#include <array>
#include <string>
#include <tuple>
#include <vector>

#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"

namespace mongo {
namespace {

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
    EXPECT_FALSE(PathArrayness::hasInvalidatedPaths(expCtx.mainCollNonArrayPaths(), current));
}

FUZZ_TEST(PathArraynessFuzz, EmptyOldTrieNeverInvalidates).WithDomains(PathOpsDomain());

void HasInvalidatedPathsMatchesManualCheck(std::vector<PathOp> oldOps,
                                           std::vector<std::vector<int>> queryComps,
                                           std::vector<PathOp> currentOps) {
    auto old = std::make_shared<PathArrayness>(buildPathArrayness(oldOps));

    ExpressionContextForTest expCtx;
    expCtx.setPathArrayness(old);
    for (const auto& comps : queryComps)
        expCtx.canMainCollPathBeArray(buildFieldPath(comps));

    PathArrayness current = buildPathArrayness(currentOps);

    ExpressionContextForTest checkExpCtx;
    checkExpCtx.setPathArrayness(std::make_shared<PathArrayness>(current));
    bool expected = false;
    for (const auto& path : expCtx.mainCollNonArrayPaths())
        if (checkExpCtx.canMainCollPathBeArray(path))
            expected = true;

    EXPECT_EQ(PathArrayness::hasInvalidatedPaths(expCtx.mainCollNonArrayPaths(), current),
              expected);
}

FUZZ_TEST(PathArraynessFuzz, HasInvalidatedPathsMatchesManualCheck)
    .WithDomains(PathOpsDomain(), QueryCompsDomain(), PathOpsDomain());

void SelfDoesNotInvalidate(std::vector<PathOp> ops, std::vector<std::vector<int>> queryComps) {
    auto old = std::make_shared<PathArrayness>(buildPathArrayness(ops));
    ExpressionContextForTest expCtx;
    expCtx.setPathArrayness(old);
    for (const auto& comps : queryComps)
        expCtx.canMainCollPathBeArray(buildFieldPath(comps));
    EXPECT_FALSE(PathArrayness::hasInvalidatedPaths(expCtx.mainCollNonArrayPaths(), *old));
}

FUZZ_TEST(PathArraynessFuzz, SelfDoesNotInvalidate)
    .WithDomains(PathOpsDomain(), QueryCompsDomain());

}  // namespace
}  // namespace mongo
