/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/db/dbdirectclient.h"

#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/instance.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/wire_version.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/log.h"

namespace mongo {

using std::unique_ptr;
using std::string;

namespace {

class DirectClientScope {
    MONGO_DISALLOW_COPYING(DirectClientScope);

public:
    explicit DirectClientScope(OperationContext* txn)
        : _txn(txn), _prev(_txn->getClient()->isInDirectClient()) {
        _txn->getClient()->setInDirectClient(true);
    }

    ~DirectClientScope() {
        _txn->getClient()->setInDirectClient(_prev);
    }

private:
    OperationContext* const _txn;
    const bool _prev;
};

}  // namespace


DBDirectClient::DBDirectClient(OperationContext* txn) : _txn(txn) {}

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
    return WireSpec::instance().minWireVersionIncoming;
}

// Returned version should match the incoming connections restrictions.
int DBDirectClient::getMaxWireVersion() {
    return WireSpec::instance().maxWireVersionIncoming;
}

ConnectionString::ConnectionType DBDirectClient::type() const {
    return ConnectionString::MASTER;
}

double DBDirectClient::getSoTimeout() const {
    return 0;
}

bool DBDirectClient::lazySupported() const {
    return true;
}

void DBDirectClient::setOpCtx(OperationContext* txn) {
    _txn = txn;
}

QueryOptions DBDirectClient::_lookupAvailableOptions() {
    // Exhaust mode is not available in DBDirectClient.
    return QueryOptions(DBClientBase::_lookupAvailableOptions() & ~QueryOption_Exhaust);
}

bool DBDirectClient::call(Message& toSend, Message& response, bool assertOk, string* actualServer) {
    DirectClientScope directClientScope(_txn);
    LastError::get(_txn->getClient()).startRequest();

    DbResponse dbResponse;
    CurOp curOp(_txn);
    assembleResponse(_txn, toSend, dbResponse, dummyHost);
    verify(!dbResponse.response.empty());
    response = std::move(dbResponse.response);

    return true;
}

void DBDirectClient::say(Message& toSend, bool isRetry, string* actualServer) {
    DirectClientScope directClientScope(_txn);
    LastError::get(_txn->getClient()).startRequest();

    DbResponse dbResponse;
    CurOp curOp(_txn);
    assembleResponse(_txn, toSend, dbResponse, dummyHost);
}

unique_ptr<DBClientCursor> DBDirectClient::query(const string& ns,
                                                 Query query,
                                                 int nToReturn,
                                                 int nToSkip,
                                                 const BSONObj* fieldsToReturn,
                                                 int queryOptions,
                                                 int batchSize) {
    return DBClientBase::query(
        ns, query, nToReturn, nToSkip, fieldsToReturn, queryOptions, batchSize);
}

const HostAndPort DBDirectClient::dummyHost("0.0.0.0", 0);

unsigned long long DBDirectClient::count(
    const string& ns, const BSONObj& query, int options, int limit, int skip) {
    BSONObj cmdObj = _countCmd(ns, query, options, limit, skip);

    NamespaceString nsString(ns);
    std::string dbname = nsString.db().toString();

    Command* countCmd = Command::findCommand("count");
    invariant(countCmd);

    std::string errmsg;
    BSONObjBuilder result;
    bool runRetval = countCmd->run(_txn, dbname, cmdObj, options, errmsg, result);
    if (!runRetval) {
        Command::appendCommandStatus(result, runRetval, errmsg);
        Status commandStatus = getStatusFromCommandResult(result.obj());
        invariant(!commandStatus.isOK());
        uassertStatusOK(commandStatus);
    }

    BSONObj resultObj = result.obj();
    return static_cast<unsigned long long>(resultObj["n"].numberLong());
}

}  // namespace mongo
