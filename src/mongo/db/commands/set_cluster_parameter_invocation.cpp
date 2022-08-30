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


#include "mongo/platform/basic.h"

#include "mongo/db/commands/set_cluster_parameter_invocation.h"

#include "mongo/db/audit.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/vector_clock.h"
#include "mongo/idl/cluster_server_parameter_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/s/write_ops/batched_command_response.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {

bool SetClusterParameterInvocation::invoke(OperationContext* opCtx,
                                           const SetClusterParameter& cmd,
                                           boost::optional<Timestamp> paramTime,
                                           const WriteConcernOptions& writeConcern) {

    BSONObj cmdParamObj = cmd.getCommandParameter();
    StringData parameterName = cmdParamObj.firstElement().fieldName();
    ServerParameter* serverParameter = _sps->get(parameterName);

    auto [query, update] = normalizeParameter(
        opCtx, cmdParamObj, paramTime, serverParameter, parameterName, cmd.getDbName().tenantId());

    BSONObjBuilder oldValueBob;
    serverParameter->append(opCtx, &oldValueBob, parameterName.toString(), boost::none);
    audit::logSetClusterParameter(opCtx->getClient(), oldValueBob.obj(), update);

    LOGV2_DEBUG(
        6432603, 2, "Updating cluster parameter on-disk", "clusterParameter"_attr = parameterName);

    return uassertStatusOK(_dbService.updateParameterOnDisk(
        opCtx, query, update, writeConcern, cmd.getDbName().tenantId()));
}

std::pair<BSONObj, BSONObj> SetClusterParameterInvocation::normalizeParameter(
    OperationContext* opCtx,
    BSONObj cmdParamObj,
    const boost::optional<Timestamp>& paramTime,
    ServerParameter* sp,
    StringData parameterName,
    const boost::optional<TenantId>& tenantId) {
    BSONElement commandElement = cmdParamObj.firstElement();
    uassert(ErrorCodes::IllegalOperation,
            "Cluster parameter value must be an object",
            BSONType::Object == commandElement.type());

    Timestamp clusterTime = paramTime ? *paramTime : _dbService.getUpdateClusterTime(opCtx);

    BSONObjBuilder updateBuilder;
    updateBuilder << "_id" << parameterName << "clusterParameterTime" << clusterTime;
    updateBuilder.appendElements(commandElement.Obj());

    BSONObj query = BSON("_id" << parameterName);
    BSONObj update = updateBuilder.obj();

    uassertStatusOK(sp->validate(update, tenantId));

    return {query, update};
}

Timestamp ClusterParameterDBClientService::getUpdateClusterTime(OperationContext* opCtx) {
    VectorClock::VectorTime vt = VectorClock::get(opCtx)->getTime();
    return vt.clusterTime().asTimestamp();
}

StatusWith<bool> ClusterParameterDBClientService::updateParameterOnDisk(
    OperationContext* opCtx,
    BSONObj query,
    BSONObj update,
    const WriteConcernOptions& writeConcern,
    const boost::optional<TenantId>& tenantId) {
    BSONObj res;

    BSONObjBuilder set;
    set.append("$set", update);
    set.doneFast();

    const auto writeConcernObj =
        BSON(WriteConcernOptions::kWriteConcernField << writeConcern.toBSON());

    try {
        _dbClient.runCommand(
            NamespaceString::makeClusterParametersNSS(tenantId).dbName().toStringWithTenantId(),
            [&] {
                write_ops::UpdateCommandRequest updateOp(
                    NamespaceString::makeClusterParametersNSS(tenantId));
                updateOp.setUpdates({[&] {
                    write_ops::UpdateOpEntry entry;
                    entry.setQ(query);
                    entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(update));
                    entry.setMulti(false);
                    entry.setUpsert(true);
                    return entry;
                }()});

                return updateOp.toBSON(writeConcernObj);
            }(),
            res);
    } catch (const DBException& ex) {
        return ex.toStatus();
    }

    BatchedCommandResponse response;
    std::string errmsg;

    if (!response.parseBSON(res, &errmsg)) {
        return Status(ErrorCodes::FailedToParse, errmsg);
    }

    return response.getNModified() > 0 || response.getN() > 0;
}

ServerParameter* ClusterParameterService::get(StringData name) {
    return ServerParameterSet::getClusterParameterSet()->get(name);
}
}  // namespace mongo
