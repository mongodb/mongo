/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/query/plan_cache/plan_cache_bm_fixture.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/util/assert_util.h"

#include <cstddef>
#include <string>

#include <benchmark/benchmark.h>

namespace mongo {
namespace {
BSONArray buildArray(int size) {
    BSONArrayBuilder builder;
    for (int i = 0; i < size; i++) {
        builder.append(i);
    }
    return builder.arr();
}

std::string getField(int index) {
    static constexpr StringData kViableChars = "abcdefghijklmnopqrstuvwxyz"_sd;
    invariant(size_t(index) < kViableChars.size());
    return std::string(1, kViableChars[index]);
}

BSONObj buildSimpleBSONSpec(int nFields, bool isMatch, bool isExclusion = false) {
    BSONObjBuilder spec;
    for (auto i = 0; i < nFields; i++) {
        int val = isMatch ? i : (isExclusion ? 0 : 1);
        spec.append(getField(i), val);
    }
    return spec.obj();
}

/**
 * Builds a filter BSON with 'nFields' simple equality predicates.
 */
BSONObj buildSimpleMatchSpec(int nFields) {
    return buildSimpleBSONSpec(nFields, true /*isMatch*/);
}

/**
 * Builds a projection BSON with 'nFields' simple inclusions or exclusions, depending on the
 * 'isExclusion' parameter.
 */
BSONObj buildSimpleProjectSpec(int nFields, bool isExclusion) {
    return buildSimpleBSONSpec(nFields, false /*isMatch*/, isExclusion);
}

BSONObj buildNestedBSONSpec(int depth, bool isExclusion, int offset) {
    std::string field;
    for (auto i = 0; i < depth - 1; i++) {
        field += getField(offset + i) += ".";
    }
    field += getField(offset + depth);

    return BSON(field << (isExclusion ? 0 : 1));
}

/**
 * Builds a BSON representing a predicate on one dotted path, where the field has depth 'depth'.
 */
BSONObj buildNestedMatchSpec(int depth, int offset = 0) {
    return buildNestedBSONSpec(depth, false /*isExclusion*/, offset);
}

/**
 * Builds a BSON representing a projection on one dotted path, where the field has depth 'depth'.
 */
BSONObj buildNestedProjectSpec(int depth, bool isExclusion, int offset = 0) {
    return buildNestedBSONSpec(depth, isExclusion, offset);
}
}  // namespace

void PlanCacheBenchmarkFixture::benchmarkMatch(benchmark::State& state) {
    auto match = buildSimpleMatchSpec(1);
    benchmarkQueryMatchProject(state, match, BSONObj());
}

void PlanCacheBenchmarkFixture::benchmarkMatchTwoFields(benchmark::State& state) {
    auto match = buildSimpleMatchSpec(2);
    benchmarkQueryMatchProject(state, match, BSONObj());
}

void PlanCacheBenchmarkFixture::benchmarkMatchTwentyFields(benchmark::State& state) {
    auto match = buildSimpleMatchSpec(20);
    benchmarkQueryMatchProject(state, match, BSONObj());
}

void PlanCacheBenchmarkFixture::benchmarkMatchDepthTwo(benchmark::State& state) {
    auto match = buildNestedMatchSpec(2);
    benchmarkQueryMatchProject(state, match, BSONObj());
}

void PlanCacheBenchmarkFixture::benchmarkMatchDepthTwenty(benchmark::State& state) {
    auto match = buildNestedMatchSpec(20);
    benchmarkQueryMatchProject(state, match, BSONObj());
}

void PlanCacheBenchmarkFixture::benchmarkMatchGtLt(benchmark::State& state) {
    auto match = fromjson("{a: {$gt: -12, $lt: 5}}");
    benchmarkQueryMatchProject(state, match, BSONObj());
}

void PlanCacheBenchmarkFixture::benchmarkMatchIn(benchmark::State& state) {
    auto match = BSON("a" << BSON("$in" << buildArray(10)));
    benchmarkQueryMatchProject(state, match, BSONObj());
}

void PlanCacheBenchmarkFixture::benchmarkMatchInLarge(benchmark::State& state) {
    auto match = BSON("a" << BSON("$in" << buildArray(1000)));
    benchmarkQueryMatchProject(state, match, BSONObj());
}

void PlanCacheBenchmarkFixture::benchmarkMatchElemMatch(benchmark::State& state) {
    auto match = fromjson("{a: {$elemMatch: {b: {$eq: 2}, c: {$lt: 3}}}}");
    benchmarkQueryMatchProject(state, match, BSONObj());
}

void PlanCacheBenchmarkFixture::benchmarkMatchSize(benchmark::State& state) {
    auto match = BSON("a" << BSON("$size" << 2));
    benchmarkQueryMatchProject(state, match, BSONObj());
}

void PlanCacheBenchmarkFixture::benchmarkMatchComplex(benchmark::State& state) {
    auto match = fromjson(
        "{$and: ["
        "{'a.b': {$not: {$eq: 2}}},"
        "{'b.c': {$lte: {$eq: 'str'}}},"
        "{$or: [{'c.d' : {$eq: 3}}, {'d.e': {$eq: 4}}]},"
        "{$or: ["
        "{'e.f': {$gt: 4}},"
        "{$and: ["
        "{'f.g': {$not: {$eq: 1}}},"
        "{'g.h': {$eq: 3}}"
        "]}"
        "]}"
        "]}");
    benchmarkQueryMatchProject(state, match, BSONObj());
}

void PlanCacheBenchmarkFixture::benchmarkProjectExclude(benchmark::State& state) {
    auto project = buildSimpleProjectSpec(1, true /*isExclusion*/);
    benchmarkQueryMatchProject(state, BSONObj(), project);
}

void PlanCacheBenchmarkFixture::benchmarkProjectInclude(benchmark::State& state) {
    auto project = buildSimpleProjectSpec(1, false /*isExclusion*/);
    benchmarkQueryMatchProject(state, BSONObj(), project);
}

void PlanCacheBenchmarkFixture::benchmarkProjectIncludeTwoFields(benchmark::State& state) {
    auto project = buildSimpleProjectSpec(2, false /*isExclusion*/);
    benchmarkQueryMatchProject(state, BSONObj(), project);
}

void PlanCacheBenchmarkFixture::benchmarkProjectIncludeTwentyFields(benchmark::State& state) {
    auto project = buildSimpleProjectSpec(20, false /*isExclusion*/);
    benchmarkQueryMatchProject(state, BSONObj(), project);
}

void PlanCacheBenchmarkFixture::benchmarkProjectIncludeDepthTwo(benchmark::State& state) {
    auto project = buildNestedProjectSpec(2, false /*isExclusion*/);
    benchmarkQueryMatchProject(state, BSONObj(), project);
}

void PlanCacheBenchmarkFixture::benchmarkProjectIncludeDepthTwenty(benchmark::State& state) {
    auto project = buildNestedProjectSpec(20, false /*isExclusion*/);
    benchmarkQueryMatchProject(state, BSONObj(), project);
}

void PlanCacheBenchmarkFixture::benchmarkMatchProjectExclude(benchmark::State& state) {
    auto match = buildSimpleMatchSpec(1);
    auto project = buildSimpleProjectSpec(1, true /*isExclusion*/);
    benchmarkQueryMatchProject(state, match, project);
}

void PlanCacheBenchmarkFixture::benchmarkMatchProjectInclude(benchmark::State& state) {
    auto match = buildSimpleMatchSpec(1);
    auto project = buildSimpleProjectSpec(1, false /*isExclusion*/);
    benchmarkQueryMatchProject(state, match, project);
}

void PlanCacheBenchmarkFixture::benchmarkMatchProjectIncludeTwoFields(benchmark::State& state) {
    auto match = buildSimpleMatchSpec(1);
    auto project = buildSimpleProjectSpec(2, false /*isExclusion*/);
    benchmarkQueryMatchProject(state, match, project);
}

void PlanCacheBenchmarkFixture::benchmarkMatchProjectIncludeTwentyFields(benchmark::State& state) {
    auto match = buildSimpleMatchSpec(1);
    auto project = buildSimpleProjectSpec(20, false /*isExclusion*/);
    benchmarkQueryMatchProject(state, match, project);
}

void PlanCacheBenchmarkFixture::benchmarkMatchProjectIncludeDepthTwo(benchmark::State& state) {
    auto match = buildSimpleMatchSpec(1);
    auto project = buildNestedProjectSpec(2, false /*isExclusion*/);
    benchmarkQueryMatchProject(state, match, project);
}

void PlanCacheBenchmarkFixture::benchmarkMatchProjectIncludeDepthTwenty(benchmark::State& state) {
    auto match = buildSimpleMatchSpec(1);
    auto project = buildNestedProjectSpec(20, false /*isExclusion*/);
    benchmarkQueryMatchProject(state, match, project);
}

void PlanCacheBenchmarkFixture::benchmarkOneStage(benchmark::State& state) {
    // Builds a match on a simple field.
    std::vector<BSONObj> pipeline;
    pipeline.push_back(BSON("$match" << buildSimpleMatchSpec(1)));
    benchmarkPipeline(state, pipeline);
}

void PlanCacheBenchmarkFixture::benchmarkTwoStages(benchmark::State& state) {
    // Builds a match on a nested field and then excludes that nested field.
    std::vector<BSONObj> pipeline;
    pipeline.push_back(BSON("$match" << buildNestedMatchSpec(3)));
    pipeline.push_back(BSON("$project" << buildNestedProjectSpec(3, true /*isExclusion*/)));
    benchmarkPipeline(state, pipeline);
}

void PlanCacheBenchmarkFixture::benchmarkTwentyStages(benchmark::State& state) {
    // Builds a sequence of alternating $match and $project stages which match on a nested field and
    // then exclude that field.
    std::vector<BSONObj> pipeline;
    for (int i = 0; i < 10; i++) {
        pipeline.push_back(BSON("$match" << buildNestedMatchSpec(3, i)));
        pipeline.push_back(BSON("$project" << buildNestedProjectSpec(3, true /*exclusion*/, i)));
    }
    benchmarkPipeline(state, pipeline);
}

}  // namespace mongo
