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


#include "mongo/db/cluster_parameters/set_cluster_parameter_invocation.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/audit.h"
#include "mongo/db/database_name.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"
#include "mongo/db/server_options.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/update/storage_validation.h"
#include "mongo/db/vector_clock/vector_clock.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/rpc/unique_message.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <string>

#include <boost/optional/optional.hpp>

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

    auto [query, update] =
        normalizeParameter(opCtx,
                           cmdParamObj,
                           clusterParameterTime,
                           previousTime,
                           serverParameter,
                           tenantId,
                           skipValidation || serverGlobalParams.clusterRole.isShardOnly());

    serverParameter->warnIfDeprecated("setClusterParameter");

    BSONObjBuilder oldValueBob;
    serverParameter->append(opCtx, &oldValueBob, std::string{parameterName}, tenantId);
    audit::logSetClusterParameter(opCtx->getClient(), oldValueBob.obj(), update, tenantId);

    LOGV2_DEBUG(
        6432603, 2, "Updating cluster parameter on-disk", "clusterParameter"_attr = parameterName);

    auto result = _dbService.updateParameterOnDisk(
        query, update, writeConcern, auth::ValidatedTenancyScope::get(opCtx));
    auto resultStatus = result.toStatus();

    // When 'previousTime' is provided, it is added to the 'query' part of the upsert command to
    // match the persisted 'clusterParameterTime' field. If in this case the upsert returns the
    // 'DuplicateKey' error, this means a concurrent operation has modified 'clusterParameterTime'.
    uassert(ErrorCodes::ConflictingOperationInProgress,
            "encountered concurrent cluster parameter update operations, please try again",
            !previousTime || resultStatus.code() != ErrorCodes::DuplicateKey);

    // Throw if the upsert command returned any errors (e.g., 'PrimarySteppedDown').
    uassertStatusOK(resultStatus);

    size_t nUpserted = result.isUpsertDetailsSet() ? result.sizeUpsertDetails() : 0;
    tassert(8454100,
            "when 'previousTime' is 'kUninitialized' a new document must be inserted",
            !(previousTime && *previousTime == LogicalTime::kUninitialized) || nUpserted == 1);

    // Return true if an oplog entry was created.
    return result.getNModified() == 1 || nUpserted == 1;
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
            BSONType::object == commandElement.type());

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
    if (previousTime) {
        // When the 'previousTime' is set, we must check that the parameter being updated has
        // 'clusterParameterTime' equal to 'previousTime'. This way we ensure there are no
        // concurrent updates. When 'previousTime' is set to 'kUninitialized', there should be no
        // cluster parameter with the given name persisted. Therefore, 'kUninitialized' should not
        // collide with any valid timestamp.
        tassert(8454101,
                "'previousTime' was set to 'kUninitialized' with a different timestamp than "
                "Timestamp(0, 0)",
                *previousTime != LogicalTime::kUninitialized ||
                    previousTime->asTimestamp() == Timestamp(0, 0));
        queryBuilder << "clusterParameterTime" << previousTime->asTimestamp();
    }

    BSONObj update = updateBuilder.obj();

    // Validate that serialized update is validate replacement update document by calling the same
    // document validation function as the update stage.
    {
        bool ignore;
        mutablebson::Document mutableUpdate(update);
        storage_validation::scanDocument(mutableUpdate, false, true, &ignore);
    }

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

BatchedCommandResponse ClusterParameterDBClientService::updateParameterOnDisk(
    BSONObj query,
    BSONObj update,
    const WriteConcernOptions& writeConcern,
    const boost::optional<auth::ValidatedTenancyScope>& validatedTenancyScope) {
    const auto tenantId = (validatedTenancyScope && validatedTenancyScope->hasTenantId())
        ? boost::make_optional(validatedTenancyScope->tenantId())
        : boost::none;
    const auto nss = NamespaceString::makeClusterParametersNSS(tenantId);
    auto request = OpMsgRequestBuilder::create(validatedTenancyScope, nss.dbName(), [&] {
        write_ops::UpdateCommandRequest updateOp(nss);
        updateOp.setUpdates({[&] {
            write_ops::UpdateOpEntry entry;
            entry.setQ(query);
            entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(update));
            entry.setMulti(false);
            entry.setUpsert(true);
            return entry;
        }()});
        updateOp.setWriteConcern(writeConcern);
        return updateOp.toBSON();
    }());

    BSONObj res = _dbClient.runCommand(request)->getCommandReply();
    BatchedCommandResponse response;
    std::string errmsg;
    auto parseResult = response.parseBSON(res, &errmsg);
    uassert(ErrorCodes::FailedToParse, errmsg, parseResult);
    return response;
}

ServerParameter* ClusterParameterService::get(StringData name) {
    return ServerParameterSet::getClusterParameterSet()->get(name);
}
}  // namespace mongo
