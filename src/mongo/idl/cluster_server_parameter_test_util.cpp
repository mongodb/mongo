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

#include "mongo/idl/cluster_server_parameter_test_util.h"

namespace mongo {
namespace cluster_server_parameter_test_util {

void upsert(BSONObj doc) {
    const auto kMajorityWriteConcern = BSON("writeConcern" << BSON("w"
                                                                   << "majority"));

    auto uniqueOpCtx = cc().makeOperationContext();
    auto* opCtx = uniqueOpCtx.get();

    BSONObj res;
    DBDirectClient client(opCtx);

    client.runCommand(
        kConfigDB.toString(),
        [&] {
            write_ops::UpdateCommandRequest updateOp(NamespaceString::kClusterParametersNamespace);
            updateOp.setUpdates({[&] {
                write_ops::UpdateOpEntry entry;
                entry.setQ(BSON(ClusterServerParameter::k_idFieldName << kCSPTest));
                entry.setU(
                    write_ops::UpdateModification::parseFromClassicUpdate(BSON("$set" << doc)));
                entry.setMulti(false);
                entry.setUpsert(true);
                return entry;
            }()});
            return updateOp.toBSON(kMajorityWriteConcern);
        }(),
        res);

    BatchedCommandResponse response;
    std::string errmsg;
    if (!response.parseBSON(res, &errmsg)) {
        uasserted(ErrorCodes::FailedToParse, str::stream() << "Failure: " << errmsg);
    }

    uassertStatusOK(response.toStatus());
    uassert(ErrorCodes::OperationFailed, "No documents upserted", response.getN());
}

void remove() {
    auto uniqueOpCtx = cc().makeOperationContext();
    auto* opCtx = uniqueOpCtx.get();

    BSONObj res;
    DBDirectClient(opCtx).runCommand(
        kConfigDB.toString(),
        [] {
            write_ops::DeleteCommandRequest deleteOp(NamespaceString::kClusterParametersNamespace);
            deleteOp.setDeletes({[] {
                write_ops::DeleteOpEntry entry;
                entry.setQ(BSON(ClusterServerParameter::k_idFieldName << kCSPTest));
                entry.setMulti(true);
                return entry;
            }()});
            return deleteOp.toBSON({});
        }(),
        res);

    BatchedCommandResponse response;
    std::string errmsg;
    if (!response.parseBSON(res, &errmsg)) {
        uasserted(ErrorCodes::FailedToParse,
                  str::stream() << "Failed to parse reply to delete command: " << errmsg);
    }
    uassertStatusOK(response.toStatus());
}

BSONObj makeClusterParametersDoc(const LogicalTime& cpTime, int intValue, StringData strValue) {
    ClusterServerParameter csp;
    csp.set_id(kCSPTest);
    csp.setClusterParameterTime(cpTime);

    ClusterServerParameterTest cspt;
    cspt.setClusterServerParameter(std::move(csp));
    cspt.setIntValue(intValue);
    cspt.setStrValue(strValue);

    return cspt.toBSON();
}

}  // namespace cluster_server_parameter_test_util
}  // namespace mongo
