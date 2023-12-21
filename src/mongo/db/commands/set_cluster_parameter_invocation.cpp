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


#include <boost/move/utility_core.hpp>
#include <string>

#include <boost/optional/optional.hpp>
#include <boost/optional/optional_io.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/audit.h"
#include "mongo/db/cluster_role.h"
#include "mongo/db/commands/set_cluster_parameter_invocation.h"
#include "mongo/db/database_name.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/ops/write_ops_parsers.h"
#include "mongo/db/server_options.h"
#include "mongo/db/vector_clock.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/rpc/unique_message.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
bool SetClusterParameterInvocation::invoke(OperationContext* opCtx,
                                           const SetClusterParameter& cmd,
                                           boost::optional<Timestamp> clusterParameterTime,
                                           boost::optional<LogicalTime> previousTime,
                                           const WriteConcernOptions& writeConcern,
                                           bool skipValidation) {

    BSONObj cmdParamObj = cmd.getCommandParameter();
    StringData parameterName = cmdParamObj.firstElement().fieldName();
    ServerParameter* serverParameter = _sps->get(parameterName);
    auto tenantId = cmd.getDbName().tenantId();

    auto [query, update] = normalizeParameter(
        opCtx,
        cmdParamObj,
        clusterParameterTime,
        previousTime,
        serverParameter,
        tenantId,
        skipValidation || serverGlobalParams.clusterRole.hasExclusively(ClusterRole::ShardServer));

    BSONObjBuilder oldValueBob;
    serverParameter->append(opCtx, &oldValueBob, parameterName.toString(), tenantId);
    audit::logSetClusterParameter(opCtx->getClient(), oldValueBob.obj(), update, tenantId);

    LOGV2_DEBUG(
        6432603, 2, "Updating cluster parameter on-disk", "clusterParameter"_attr = parameterName);

    if (previousTime) {
        // Check for concurrent update operations.
        // The new parameter document was either inserted or updated.
        auto result = uassertStatusOK(
            (*previousTime == LogicalTime::kUninitialized)
                ? _dbService.insertParameterOnDisk(opCtx, update, writeConcern, tenantId)
                : _dbService.updateParameterOnDisk(
                      opCtx, query, update, false /* upsert */, writeConcern, tenantId));
        uassert(ErrorCodes::ConflictingOperationInProgress,
                "encountered concurrent cluster parameter update operations, please try again",
                result.getN() == 1);
        return true;
    }

    auto result = uassertStatusOK(_dbService.updateParameterOnDisk(
        opCtx, query, update, true /* upsert */, writeConcern, tenantId));
    return result.getN() == 1 || result.getNModified() == 1;
}

std::pair<BSONObj, BSONObj> SetClusterParameterInvocation::normalizeParameter(
    OperationContext* opCtx,
    BSONObj cmdParamObj,
    boost::optional<Timestamp> clusterParameterTime,
    boost::optional<LogicalTime> previousTime,
    ServerParameter* sp,
    const boost::optional<TenantId>& tenantId,
    bool skipValidation) {
    BSONElement commandElement = cmdParamObj.firstElement();
    uassert(ErrorCodes::BadValue,
            "Cluster parameter value must be an object",
            BSONType::Object == commandElement.type());

    uassert(ErrorCodes::BadValue,
            str::stream() << "Server parameter: '" << sp->name() << "' is disabled",
            skipValidation || sp->isEnabled());

    Timestamp clusterTime =
        clusterParameterTime ? *clusterParameterTime : _dbService.getUpdateClusterTime(opCtx);
    BSONObjBuilder updateBuilder;
    updateBuilder << "_id" << sp->name() << "clusterParameterTime" << clusterTime;
    updateBuilder.appendElements(commandElement.Obj());

    BSONObjBuilder queryBuilder;
    queryBuilder << "_id" << sp->name();
    if (previousTime && *previousTime != LogicalTime::kUninitialized) {
        // When the 'previousTime' is set, we must check that the parameter being updated has
        // 'clusterParameterTime' equal to 'previousTime'. This way we ensure there are no
        // concurrent updates.
        queryBuilder << "clusterParameterTime" << previousTime->asTimestamp();
    }

    BSONObj update = updateBuilder.obj();
    BSONObj query = queryBuilder.obj();

    if (!skipValidation) {
        uassertStatusOK(sp->validate(update, tenantId));
    }

    return {query, update};
}

Timestamp ClusterParameterDBClientService::getUpdateClusterTime(OperationContext* opCtx) {
    VectorClock::VectorTime vt = VectorClock::get(opCtx)->getTime();
    return vt.clusterTime().asTimestamp();
}

StatusWith<BatchedCommandResponse> ClusterParameterDBClientService::updateParameterOnDisk(
    OperationContext* opCtx,
    BSONObj query,
    BSONObj update,
    bool upsert,
    const WriteConcernOptions& writeConcern,
    const boost::optional<TenantId>& tenantId) {
    BSONObj res;

    BSONObjBuilder set;
    set.append("$set", update);
    set.doneFast();

    const auto writeConcernObj =
        BSON(WriteConcernOptions::kWriteConcernField << writeConcern.toBSON());

    const auto nss = NamespaceString::makeClusterParametersNSS(tenantId);

    try {
        auto opMsgRequest = OpMsgRequestBuilder::create(nss.dbName(), [&] {
            write_ops::UpdateCommandRequest updateOp(nss);
            updateOp.setUpdates({[&] {
                write_ops::UpdateOpEntry entry;
                entry.setQ(query);
                entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(update));
                entry.setMulti(false);
                entry.setUpsert(upsert);
                return entry;
            }()});

            return updateOp.toBSON(writeConcernObj);
        }());
        res = _dbClient.runCommand(opMsgRequest)->getCommandReply();
    } catch (const DBException& ex) {
        return ex.toStatus();
    }

    BatchedCommandResponse response;
    std::string errmsg;

    auto parseResult = response.parseBSON(res, &errmsg);
    uassert(ErrorCodes::FailedToParse, errmsg, parseResult);

    return std::move(response);
}

StatusWith<BatchedCommandResponse> ClusterParameterDBClientService::insertParameterOnDisk(
    OperationContext* opCtx,
    BSONObj update,
    const WriteConcernOptions& writeConcern,
    const boost::optional<TenantId>& tenantId) {
    BSONObj res;

    const auto writeConcernObj =
        BSON(WriteConcernOptions::kWriteConcernField << writeConcern.toBSON());

    const auto nss = NamespaceString::makeClusterParametersNSS(tenantId);

    try {
        auto opMsgRequest = OpMsgRequestBuilder::create(nss.dbName(), [&] {
            write_ops::InsertCommandRequest insertOp(nss, {update});
            return insertOp.toBSON(writeConcernObj);
        }());
        res = _dbClient.runCommand(opMsgRequest)->getCommandReply();
    } catch (const DBException& ex) {
        return ex.toStatus();
    }

    BatchedCommandResponse response;
    std::string errmsg;

    auto parseResult = response.parseBSON(res, &errmsg);
    uassert(ErrorCodes::FailedToParse, errmsg, parseResult);

    return std::move(response);
}

ServerParameter* ClusterParameterService::get(StringData name) {
    return ServerParameterSet::getClusterParameterSet()->get(name);
}
}  // namespace mongo
