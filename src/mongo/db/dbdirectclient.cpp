/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/dbdirectclient.h"

#include <boost/move/utility_core.hpp>
#include <utility>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/db/wire_version.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/rpc/unique_message.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo {

using std::string;
using std::unique_ptr;

namespace {

class DirectClientScope {
    DirectClientScope(const DirectClientScope&) = delete;
    DirectClientScope& operator=(const DirectClientScope&) = delete;

public:
    explicit DirectClientScope(OperationContext* opCtx)
        : _opCtx(opCtx), _prev(_opCtx->getClient()->isInDirectClient()) {
        _opCtx->getClient()->setInDirectClient(true);
    }

    ~DirectClientScope() {
        _opCtx->getClient()->setInDirectClient(_prev);
    }

private:
    OperationContext* const _opCtx;
    const bool _prev;
};

}  // namespace


DBDirectClient::DBDirectClient(OperationContext* opCtx) : _opCtx(opCtx) {}

void DBDirectClient::_auth(const BSONObj& params) {
    uasserted(2625701, "DBDirectClient should not authenticate");
}

bool DBDirectClient::isFailed() const {
    return false;
}

bool DBDirectClient::isStillConnected() {
    return true;
}

std::string DBDirectClient::toString() const {
    return "DBDirectClient";
}

std::string DBDirectClient::getServerAddress() const {
    return "localhost";  // TODO: should this have the port?
}

// Returned version should match the incoming connections restrictions.
int DBDirectClient::getMinWireVersion() {
    return WireSpec::getWireSpec(_opCtx->getServiceContext())
        .get()
        ->incomingExternalClient.minWireVersion;
}

// Returned version should match the incoming connections restrictions.
int DBDirectClient::getMaxWireVersion() {
    return WireSpec::getWireSpec(_opCtx->getServiceContext())
        .get()
        ->incomingExternalClient.maxWireVersion;
}

bool DBDirectClient::isReplicaSetMember() const {
    auto const* replCoord = repl::ReplicationCoordinator::get(_opCtx);
    return replCoord && replCoord->getSettings().isReplSet();
}

ConnectionString::ConnectionType DBDirectClient::type() const {
    return ConnectionString::ConnectionType::kStandalone;
}

double DBDirectClient::getSoTimeout() const {
    return 0;
}

namespace {
DbResponse loopbackBuildResponse(OperationContext* const opCtx, Message& toSend) {
    auth::ValidatedTenancyScopeGuard tenancyStasher(opCtx);
    DirectClientScope directClientScope(opCtx);
    StashTransactionResourcesForDBDirect stashedTxnResources(opCtx);

    CurOp curOp;
    curOp.push(opCtx);

    toSend.header().setId(nextMessageId());
    toSend.header().setResponseToMsgId(0);
    IgnoreAPIParametersBlock ignoreApiParametersBlock(opCtx);
    return opCtx->getService()->getServiceEntryPoint()->handleRequest(opCtx, toSend).get();
}
}  // namespace

Message DBDirectClient::_call(Message& toSend, string* actualServer) {
    auto dbResponse = loopbackBuildResponse(_opCtx, toSend);
    invariant(!dbResponse.response.empty());
    return std::move(dbResponse.response);
}

auth::ValidatedTenancyScope DBDirectClient::_createInnerRequestVTS(
    const boost::optional<TenantId>& tenantId) const {
    const auto vtsOnOpCtx = auth::ValidatedTenancyScope::get(_opCtx);
    if (tenantId && vtsOnOpCtx && vtsOnOpCtx->hasTenantId()) {
        invariant(vtsOnOpCtx->tenantId() == tenantId,
                  str::stream() << "The tenant id '" << vtsOnOpCtx->tenantId()
                                << "' on opCtx does not match the requested tenant id "
                                << tenantId);
    }
    return DBClientBase::_createInnerRequestVTS(tenantId);
}

void DBDirectClient::say(Message& toSend, bool isRetry, string* actualServer) {
    auto dbResponse = loopbackBuildResponse(_opCtx, toSend);
    invariant(dbResponse.response.empty());
}

std::unique_ptr<DBClientCursor> DBDirectClient::find(FindCommandRequest findRequest,
                                                     const ReadPreferenceSetting& readPref,
                                                     ExhaustMode exhaustMode) {
    invariant(!findRequest.getReadConcern(),
              "passing readConcern to DBDirectClient::find() is not supported as it has to use the "
              "parent operation's readConcern");
    return DBClientBase::find(std::move(findRequest), readPref, exhaustMode);
}

write_ops::FindAndModifyCommandReply DBDirectClient::findAndModify(
    const write_ops::FindAndModifyCommandRequest& findAndModify) {
    auto request = findAndModify.serialize({});
    request.validatedTenancyScope = _createInnerRequestVTS(findAndModify.getDbName().tenantId());
    auto response = runCommand(std::move(request));
    return FindAndModifyOp::parseResponse(response->getCommandReply());
}

write_ops::InsertCommandReply DBDirectClient::insert(
    const write_ops::InsertCommandRequest& insert) {
    auto request = insert.serialize({});
    request.validatedTenancyScope = _createInnerRequestVTS(insert.getDbName().tenantId());
    auto response = runCommand(request);
    return InsertOp::parseResponse(response->getCommandReply());
}

write_ops::UpdateCommandReply DBDirectClient::update(
    const write_ops::UpdateCommandRequest& update) {
    auto request = update.serialize({});
    request.validatedTenancyScope = _createInnerRequestVTS(update.getDbName().tenantId());

    auto response = runCommand(request);
    return UpdateOp::parseResponse(response->getCommandReply());
}

write_ops::DeleteCommandReply DBDirectClient::remove(
    const write_ops::DeleteCommandRequest& remove) {
    auto request = remove.serialize({});
    request.validatedTenancyScope = _createInnerRequestVTS(remove.getDbName().tenantId());

    auto response = runCommand(request);
    return DeleteOp::parseResponse(response->getCommandReply());
}

long long DBDirectClient::count(const NamespaceStringOrUUID nsOrUuid,
                                const BSONObj& query,
                                int options,
                                int limit,
                                int skip,
                                boost::optional<BSONObj> readConcernObj) {
    invariant(!readConcernObj,
              "passing readConcern to DBDirectClient functions is not supported as it has to use "
              "the parent operation's readConcern");
    BSONObj cmdObj = _countCmd(nsOrUuid, query, options, limit, skip, boost::none);
    auto request = OpMsgRequestBuilder::createWithValidatedTenancyScope(
        nsOrUuid.dbName(), _createInnerRequestVTS(nsOrUuid.dbName().tenantId()), cmdObj);

    // Calls runCommand instead of runCommandDirectly to ensure the tenant inforamtion of this
    // command gets validated and is used for parsing the command request.
    auto response = runCommand(request);
    auto& result = response->getCommandReply();

    uassertStatusOK(getStatusFromCommandResult(result));
    return result["n"].numberLong();
}

}  // namespace mongo
