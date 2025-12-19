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

#include "mongo/db/query/compiler/optimizer/join/agg_join_model_fixture.h"

#include "mongo/db/pipeline/optimization/optimize.h"

namespace mongo::join_ordering {
std::string AggJoinModelFixture::toString(const std::unique_ptr<Pipeline>& pipeline) {
    auto bson = pipeline->serializeToBson();
    BSONArrayBuilder ba{};
    ba.append(bson.begin(), bson.end());
    return toString(BSON("pipeline" << ba.arr()));
}

std::vector<BSONObj> AggJoinModelFixture::pipelineFromJsonArray(StringData jsonArray) {
    auto inputBson = fromjson("{pipeline: " + jsonArray + "}");
    ASSERT_EQUALS(inputBson["pipeline"].type(), BSONType::array);
    std::vector<BSONObj> rawPipeline;
    for (auto&& stageElem : inputBson["pipeline"].Array()) {
        ASSERT_EQUALS(stageElem.type(), BSONType::object);
        rawPipeline.push_back(stageElem.embeddedObject().getOwned());
    }
    return rawPipeline;
}

std::unique_ptr<Pipeline> AggJoinModelFixture::makePipeline(std::vector<BSONObj> bsonStages,
                                                            std::vector<StringData> collNames) {
    stdx::unordered_set<NamespaceString> secondaryNamespaces;
    for (auto&& collName : collNames) {
        secondaryNamespaces.insert(
            NamespaceString::createNamespaceString_forTest("test", collName));
    }
    auto expCtx = getExpCtx();
    expCtx->addResolvedNamespaces(secondaryNamespaces);
    auto pipeline = Pipeline::parse(bsonStages, expCtx);
    pipeline_optimization::optimizePipeline(*pipeline);

    return pipeline;
}

std::unique_ptr<Pipeline> AggJoinModelFixture::makePipeline(StringData query,
                                                            std::vector<StringData> collNames) {
    const auto bsonStages = pipelineFromJsonArray(query);
    return makePipeline(std::move(bsonStages), std::move(collNames));
}

std::unique_ptr<Pipeline> AggJoinModelFixture::makePipelineOfSize(size_t numJoins) {
    std::vector<BSONObj> stages;
    for (size_t i = 0; i != numJoins; ++i) {
        std::string asField = str::stream() << "from" << i;
        stages.emplace_back(
            BSON("$lookup" << BSON("from" << "A" << "localField" << "a" << "foreignField" << "b"
                                          << "as" << asField)));
        stages.emplace_back(BSON("$unwind" << ("$" + asField)));
    }
    return makePipeline(std::move(stages), {"A"});
}
}  // namespace mongo::join_ordering
