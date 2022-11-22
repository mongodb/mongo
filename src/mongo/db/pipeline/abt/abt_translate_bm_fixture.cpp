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

#include "mongo/db/pipeline/abt/abt_translate_bm_fixture.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/json.h"

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

void ABTTranslateBenchmarkFixture::benchmarkMatch(benchmark::State& state) {
    auto match = buildSimpleMatchSpec(1);
    benchmarkABTTranslate(state, match, BSONObj());
}
void ABTTranslateBenchmarkFixture::benchmarkMatchTwoFields(benchmark::State& state) {
    auto match = buildSimpleMatchSpec(2);
    benchmarkABTTranslate(state, match, BSONObj());
}

void ABTTranslateBenchmarkFixture::benchmarkMatchTwentyFields(benchmark::State& state) {
    auto match = buildSimpleMatchSpec(20);
    benchmarkABTTranslate(state, match, BSONObj());
}

void ABTTranslateBenchmarkFixture::benchmarkMatchDepthTwo(benchmark::State& state) {
    auto match = buildNestedMatchSpec(2);
    benchmarkABTTranslate(state, match, BSONObj());
}

void ABTTranslateBenchmarkFixture::benchmarkMatchDepthTwenty(benchmark::State& state) {
    auto match = buildNestedMatchSpec(20);
    benchmarkABTTranslate(state, match, BSONObj());
}

void ABTTranslateBenchmarkFixture::benchmarkMatchGtLt(benchmark::State& state) {
    auto match = fromjson("{a: {$gt: -12, $lt: 5}}");
    benchmarkABTTranslate(state, match, BSONObj());
}

void ABTTranslateBenchmarkFixture::benchmarkMatchIn(benchmark::State& state) {
    auto match = BSON("a" << BSON("$in" << buildArray(10)));
    benchmarkABTTranslate(state, match, BSONObj());
}

void ABTTranslateBenchmarkFixture::benchmarkMatchInLarge(benchmark::State& state) {
    auto match = BSON("a" << BSON("$in" << buildArray(1000)));
    benchmarkABTTranslate(state, match, BSONObj());
}

void ABTTranslateBenchmarkFixture::benchmarkMatchElemMatch(benchmark::State& state) {
    auto match = fromjson("{a: {$elemMatch: {b: {$eq: 2}, c: {$lt: 3}}}}");
    benchmarkABTTranslate(state, match, BSONObj());
}

void ABTTranslateBenchmarkFixture::benchmarkMatchComplex(benchmark::State& state) {
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
        "]}}");
    benchmarkABTTranslate(state, match, BSONObj());
}

void ABTTranslateBenchmarkFixture::benchmarkProjectExclude(benchmark::State& state) {
    auto project = buildSimpleProjectSpec(1, true /*isExclusion*/);
    benchmarkABTTranslate(state, BSONObj(), project);
}

void ABTTranslateBenchmarkFixture::benchmarkProjectInclude(benchmark::State& state) {
    auto project = buildSimpleProjectSpec(1, false /*isExclusion*/);
    benchmarkABTTranslate(state, BSONObj(), project);
}

void ABTTranslateBenchmarkFixture::benchmarkProjectIncludeTwoFields(benchmark::State& state) {
    auto project = buildSimpleProjectSpec(2, false /*isExclusion*/);
    benchmarkABTTranslate(state, BSONObj(), project);
}

void ABTTranslateBenchmarkFixture::benchmarkProjectIncludeTwentyFields(benchmark::State& state) {
    auto project = buildSimpleProjectSpec(20, false /*isExclusion*/);
    benchmarkABTTranslate(state, BSONObj(), project);
}

void ABTTranslateBenchmarkFixture::benchmarkProjectIncludeDepthTwo(benchmark::State& state) {
    auto project = buildNestedProjectSpec(2, false /*isExclusion*/);
    benchmarkABTTranslate(state, BSONObj(), project);
}

void ABTTranslateBenchmarkFixture::benchmarkProjectIncludeDepthTwenty(benchmark::State& state) {
    auto project = buildNestedProjectSpec(20, false /*isExclusion*/);
    benchmarkABTTranslate(state, BSONObj(), project);
}

void ABTTranslateBenchmarkFixture::benchmarkMatchBitsAllClear(benchmark::State& state) {
    // $bitsAllClear is an unsupported match expression.
    auto match = fromjson("{unsupportedExpr: {$bitsAllClear: [1, 5]}}");
    benchmarkABTTranslate(state, match, BSONObj());
}

void ABTTranslateBenchmarkFixture::benchmarkMatchManyEqualitiesThenBitsAllClear(
    benchmark::State& state) {
    // Build a match expression with simple equalities on several fields, then include one
    // unsupported match expression ($bitsAllClear).
    auto match = buildSimpleMatchSpec(20);
    match = match.addField(fromjson("{unsupportedExpr: {$bitsAllClear: [1, 5]}}").firstElement());
    benchmarkABTTranslate(state, match, BSONObj());
}

void ABTTranslateBenchmarkFixture::benchmarkProjectRound(benchmark::State& state) {
    // $round is an unsupported agg expression.
    auto project = fromjson("{unsupportedExpr: {$round: ['$value', 1]}}");
    benchmarkABTTranslate(state, BSONObj(), project);
}

void ABTTranslateBenchmarkFixture::benchmarkProjectManyInclusionsThenRound(
    benchmark::State& state) {
    // Build a projection with simple inclusions on several fields, then include add an unsupported
    // computed expression ($round).
    auto project = buildSimpleProjectSpec(20, false /*isExclusion*/);
    project =
        project.addField(fromjson("{unsupportedExpr: {$round: ['$value', 1]}}").firstElement());
    benchmarkABTTranslate(state, BSONObj(), project);
}

void ABTTranslateBenchmarkFixture::benchmarkTwoStages(benchmark::State& state) {
    // Builds a match on a nested field and then excludes that nested field.
    std::vector<BSONObj> pipeline;
    pipeline.push_back(BSON("$match" << buildNestedMatchSpec(3)));
    pipeline.push_back(BSON("$project" << buildNestedProjectSpec(3, true /*isExclusion*/)));
    benchmarkABTTranslate(state, pipeline);
}

void ABTTranslateBenchmarkFixture::benchmarkTwentyStages(benchmark::State& state) {
    // Builds a sequence of alternating $match and $project stages which match on a nested field and
    // then exclude that field.
    std::vector<BSONObj> pipeline;
    for (int i = 0; i < 10; i++) {
        pipeline.push_back(BSON("$match" << buildNestedMatchSpec(3, i)));
        pipeline.push_back(BSON("$project" << buildNestedProjectSpec(3, true /*exclusion*/, i)));
    }
    benchmarkABTTranslate(state, pipeline);
}

void ABTTranslateBenchmarkFixture::benchmarkSampleStage(benchmark::State& state) {
    std::vector<BSONObj> pipeline{fromjson("{$sample: {size: 4}}")};
    benchmarkABTTranslate(state, pipeline);
}
void ABTTranslateBenchmarkFixture::benchmarkManyStagesThenSample(benchmark::State& state) {
    // Builds a sequence of alternating $match and $project stages which match on a nested field and
    // then exclude that field.
    std::vector<BSONObj> pipeline;
    for (int i = 0; i < 10; i++) {
        pipeline.push_back(BSON("$match" << buildNestedMatchSpec(3, i)));
        pipeline.push_back(BSON("$project" << buildNestedProjectSpec(3, true /*exclusion*/, i)));
    }

    // Finally, add an unsupported stage.
    pipeline.push_back(fromjson("{$sample: {size: 4}}"));
    benchmarkABTTranslate(state, pipeline);
}

void ABTTranslateBenchmarkFixture::benchmarkSampleThenManyStages(benchmark::State& state) {
    std::vector<BSONObj> pipeline;

    // Add an unsupported stage.
    pipeline.push_back(fromjson("{$sample: {size: 4}}"));

    // Builds a sequence of alternating $match and $project stages which match on a nested field and
    // then exclude that field.
    for (int i = 0; i < 10; i++) {
        pipeline.push_back(BSON("$match" << buildNestedMatchSpec(3, i)));
        pipeline.push_back(BSON("$project" << buildNestedProjectSpec(3, true /*exclusion*/, i)));
    }

    benchmarkABTTranslate(state, pipeline);
}

}  // namespace mongo
