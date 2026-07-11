// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/optimizer/join/agg_join_model_fixture.h"

#include <string_view>

namespace mongo::join_ordering {
std::string AggJoinModelFixture::toString(const std::unique_ptr<Pipeline>& pipeline) {
    auto bson = pipeline->serializeToBson();
    BSONArrayBuilder ba{};
    ba.append(bson.begin(), bson.end());
    return toString(BSON("pipeline" << ba.arr()));
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
