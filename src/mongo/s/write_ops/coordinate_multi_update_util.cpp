/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/s/write_ops/coordinate_multi_update_util.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/coordinate_multi_update_gen.h"

namespace mongo {
namespace coordinate_multi_update_util {

namespace {
auto getCommandNameForOp(const BatchItemRef& op) {
    switch (op.getOpType()) {
        case BatchedCommandRequest::BatchType_Update:
            return "update";
        case BatchedCommandRequest::BatchType_Delete:
            return "delete";
        default:
            MONGO_UNREACHABLE;
    }
}

auto getOpsFieldNameForOp(const BatchItemRef& op) {
    switch (op.getOpType()) {
        case BatchedCommandRequest::BatchType_Update:
            return "updates";
        case BatchedCommandRequest::BatchType_Delete:
            return "deletes";
        default:
            MONGO_UNREACHABLE;
    }
}

auto getOpAsBson(const BatchItemRef& op) {
    switch (op.getOpType()) {
        case BatchedCommandRequest::BatchType_Update:
            return op.getUpdateRef().toBSON();
        case BatchedCommandRequest::BatchType_Delete:
            return op.getDeleteRef().toBSON();
        default:
            MONGO_UNREACHABLE;
    }
}

auto getRequestBson(const BatchedCommandRequest& clientRequest) {
    switch (clientRequest.getBatchType()) {
        case BatchedCommandRequest::BatchType_Update:
            return clientRequest.getUpdateRequest().toBSON();
        case BatchedCommandRequest::BatchType_Delete:
            return clientRequest.getDeleteRequest().toBSON();
        default:
            MONGO_UNREACHABLE;
    }
};

BatchItemRef getWriteOpFromBatch(BatchWriteOp& batchOp, const TargetedBatchMap& childBatches) {
    return batchOp.getWriteOp(getWriteOpIndex(childBatches)).getWriteItem();
}

BulkWriteCRUDOp getWriteOpFromBulk(const BulkWriteCommandRequest& bulkOp,
                                   const TargetedBatchMap& childBatches) {
    return BulkWriteCRUDOp{bulkOp.getOps()[getWriteOpIndex(childBatches)]};
}

BSONObj makeCommandForOp(BatchWriteOp& batchOp,
                         const TargetedBatchMap& childBatches,
                         const BatchedCommandRequest& clientRequest) {
    auto op = getWriteOpFromBatch(batchOp, childBatches);
    BSONObjBuilder bob;
    clientRequest.getNS().serializeCollectionName(&bob, getCommandNameForOp(op));
    bob.append(getOpsFieldNameForOp(op), BSON_ARRAY(getOpAsBson(op)));
    bob.appendElementsUnique(getRequestBson(clientRequest));
    return bob.obj();
}

BSONObj makeCommandForOp(bulk_write_exec::BulkWriteOp& bulkWriteOp,
                         const BulkWriteCRUDOp& bulkCrudOp) {
    return bulkWriteOp.getClientRequest().toBSON().addField(
        BSON("ops" << BSON_ARRAY(bulkCrudOp.toBSON())).firstElement());
}

BSONObj executeCoordinateMultiUpdate(OperationContext* opCtx,
                                     const NamespaceString& nss,
                                     BSONObj writeCommand) {
    ShardsvrCoordinateMultiUpdate coordinateCommand{nss};
    coordinateCommand.setDbName(nss.dbName());
    coordinateCommand.setUuid(UUID::gen());
    coordinateCommand.setCommand(std::move(writeCommand));

    auto catalogCache = Grid::get(opCtx)->catalogCache();
    const auto dbInfo = uassertStatusOK(catalogCache->getDatabase(opCtx, nss.dbName()));

    auto response = executeCommandAgainstDatabasePrimary(
        opCtx,
        DatabaseName::kAdmin,
        dbInfo,
        CommandHelpers::appendMajorityWriteConcern(coordinateCommand.toBSON(),
                                                   opCtx->getWriteConcern()),
        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
        Shard::RetryPolicy::kIdempotent);

    uassertStatusOK(AsyncRequestsSender::Response::getEffectiveStatus(response));
    auto parsed = ShardsvrCoordinateMultiUpdateResponse::parse(
        IDLParserContext{"coordinate_multi_update_util::executeCoordinateMultiUpdate"},
        response.swResponse.getValue().data);
    invariant(parsed.getResult());
    return *parsed.getResult();
}

BatchedCommandResponse parseBatchedResponse(const BSONObj& response) {
    std::string errMsg;
    BatchedCommandResponse result;
    auto success = result.parseBSON(response, &errMsg);
    uassert(ErrorCodes::FailedToParse, errMsg, success);
    return result;
}

BulkWriteCommandReply parseBulkResponse(const BSONObj& response) {
    return BulkWriteCommandReply::parse(
        IDLParserContext{"coordinate_multi_update_util::parseBulkResponse"}, response);
}

}  // namespace

int getWriteOpIndex(const TargetedBatchMap& childBatches) {
    // There can be multiple entries in childBatches if the multi write targets multiple shards, but
    // each of these entries is the same write.
    const auto& writes = childBatches.begin()->second->getWrites();
    invariant(writes.size() == 1);
    return writes.front()->writeOpRef.first;
}

BatchedCommandResponse executeCoordinateMultiUpdate(OperationContext* opCtx,
                                                    BatchWriteOp& batchOp,
                                                    const TargetedBatchMap& childBatches,
                                                    const BatchedCommandRequest& clientRequest) {
    try {
        return parseBatchedResponse(executeCoordinateMultiUpdate(
            opCtx, clientRequest.getNS(), makeCommandForOp(batchOp, childBatches, clientRequest)));
    } catch (const DBException& e) {
        BatchedCommandResponse result;
        result.setStatus(e.toStatus());
        return result;
    }
}

BulkWriteCommandReply executeCoordinateMultiUpdate(OperationContext* opCtx,
                                                   TargetedBatchMap& childBatches,
                                                   bulk_write_exec::BulkWriteOp& bulkWriteOp) {
    try {
        const auto& request = bulkWriteOp.getClientRequest();
        auto op = getWriteOpFromBulk(request, childBatches);
        auto nss = request.getNsInfo()[op.getNsInfoIdx()].getNs();
        return parseBulkResponse(
            executeCoordinateMultiUpdate(opCtx, nss, makeCommandForOp(bulkWriteOp, op)));
    } catch (const DBException& e) {
        return bulk_write_exec::createEmulatedErrorReply(e.toStatus(), 1, boost::none);
    }
}

}  // namespace coordinate_multi_update_util
}  // namespace mongo
