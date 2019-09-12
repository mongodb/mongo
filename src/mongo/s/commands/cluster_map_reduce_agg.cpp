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

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/map_reduce_gen.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/getmore_request.h"
#include "mongo/s/commands/cluster_map_reduce_agg.h"

namespace mongo {

// Exhaust the cursor from the aggregation response and extract results and statistics.
std::vector<BSONObj> getAllAggregationResults(OperationContext* opCtx,
                                              const std::string& dbname,
                                              CursorResponse& response) {
    CursorId cursorId = response.getCursorId();
    auto fullBatch = response.releaseBatch();
    while (cursorId != 0) {
        GetMoreRequest request(
            response.getNSS(), cursorId, boost::none, boost::none, boost::none, boost::none);
        BSONObj getMoreResponse = CommandHelpers::runCommandDirectly(
            opCtx, OpMsgRequest::fromDBAndBody(dbname, request.toBSON()));
        auto getMoreCursorResponse = CursorResponse::parseFromBSONThrowing(getMoreResponse);
        auto nextBatch = getMoreCursorResponse.releaseBatch();
        fullBatch.insert(fullBatch.end(), nextBatch.begin(), nextBatch.end());
        cursorId = getMoreCursorResponse.getCursorId();
    }
    return fullBatch;
}

bool runAggregationMapReduce(OperationContext* opCtx,
                             const std::string& dbname,
                             const BSONObj& cmd,
                             std::string& errmsg,
                             BSONObjBuilder& result) {
    // Pretend we have built the appropriate pipeline and aggregation request.
    auto mrRequest = MapReduce::parse(IDLParserErrorContext("MapReduce"), cmd);
    const BSONObj aggRequest =
        fromjson(str::stream() << "{aggregate: '" << mrRequest.getNamespace().coll()
                               << "', pipeline: [ { $group: { _id: { user: \"$user\" },"
                               << "count: { $sum: 1 } } } ], cursor: {}}");
    BSONObj aggResult = CommandHelpers::runCommandDirectly(
        opCtx, OpMsgRequest::fromDBAndBody(dbname, std::move(aggRequest)));

    bool inMemory = mrRequest.getOutOptions().getOutputType() == OutputType::InMemory;
    std::string outColl = mrRequest.getOutOptions().getCollectionName();
    // Either inline response specified or we have an output collection.
    invariant(inMemory ^ !outColl.empty());

    auto cursorResponse = CursorResponse::parseFromBSONThrowing(aggResult);
    auto completeBatch = getAllAggregationResults(opCtx, dbname, cursorResponse);
    [[maybe_unused]] CursorResponse completeCursor(
        cursorResponse.getNSS(), cursorResponse.getCursorId(), std::move(completeBatch));

    return true;
}

}  // namespace mongo
