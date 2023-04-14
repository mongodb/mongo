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

#include "mongo/platform/basic.h"

#include "mongo/dbtests/mock/mock_remote_db_server.h"

#include <memory>
#include <tuple>

#include "mongo/db/exec/projection_executor_builder.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/projection_parser.h"
#include "mongo/dbtests/mock/mock_dbclient_connection.h"
#include "mongo/rpc/metadata.h"
#include "mongo/rpc/op_msg_rpc_impls.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/socket_exception.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

using std::string;
using std::vector;

namespace mongo {

MockRemoteDBServer::CircularBSONIterator::CircularBSONIterator(
    const vector<StatusWith<BSONObj>>& replyVector) {
    for (auto iter = replyVector.begin(); iter != replyVector.end(); ++iter) {
        _replyObjs.push_back(iter->isOK() ? StatusWith(iter->getValue().copy()) : *iter);
    }

    _iter = _replyObjs.begin();
}

StatusWith<BSONObj> MockRemoteDBServer::CircularBSONIterator::next() {
    verify(_iter != _replyObjs.end());

    StatusWith<BSONObj> reply = _iter->isOK() ? StatusWith(_iter->getValue().copy()) : *_iter;
    ++_iter;

    if (_iter == _replyObjs.end()) {
        _iter = _replyObjs.begin();
    }

    return reply;
}

MockRemoteDBServer::MockRemoteDBServer(const string& hostAndPort)
    : _isRunning(true),
      _hostAndPort(HostAndPort(hostAndPort)),
      _delayMilliSec(0),
      _cmdCount(0),
      _queryCount(0),
      _instanceID(0) {
    insert(IdentityNS, BSON(HostField(hostAndPort)));
    setCommandReply("dbStats", BSON(HostField(hostAndPort)));
}

MockRemoteDBServer::~MockRemoteDBServer() {}

void MockRemoteDBServer::setDelay(long long milliSec) {
    scoped_spinlock sLock(_lock);
    _delayMilliSec = milliSec;
}

void MockRemoteDBServer::shutdown() {
    scoped_spinlock sLock(_lock);
    _isRunning = false;
}

void MockRemoteDBServer::reboot() {
    scoped_spinlock sLock(_lock);
    _isRunning = true;
    _instanceID++;
}

MockRemoteDBServer::InstanceID MockRemoteDBServer::getInstanceID() const {
    scoped_spinlock sLock(_lock);
    return _instanceID;
}

bool MockRemoteDBServer::isRunning() const {
    scoped_spinlock sLock(_lock);
    return _isRunning;
}

void MockRemoteDBServer::setCommandReply(const string& cmdName,
                                         const StatusWith<mongo::BSONObj>& replyObj) {
    vector<StatusWith<BSONObj>> replySequence;
    replySequence.push_back(replyObj);
    setCommandReply(cmdName, replySequence);
}

void MockRemoteDBServer::setCommandReply(const string& cmdName,
                                         const vector<StatusWith<BSONObj>>& replySequence) {
    scoped_spinlock sLock(_lock);
    _cmdMap[cmdName].reset(new CircularBSONIterator(replySequence));
}

void MockRemoteDBServer::insert(const NamespaceString& nss, BSONObj obj) {
    scoped_spinlock sLock(_lock);

    vector<BSONObj>& mockCollection = _dataMgr[nss.ns().toString()];
    mockCollection.push_back(obj.copy());
}

void MockRemoteDBServer::remove(const NamespaceString& nss, const BSONObj&) {
    scoped_spinlock sLock(_lock);
    auto ns = nss.ns().toString();
    if (_dataMgr.count(ns) == 0) {
        return;
    }

    _dataMgr.erase(ns);
}

void MockRemoteDBServer::assignCollectionUuid(StringData ns, const mongo::UUID& uuid) {
    scoped_spinlock sLock(_lock);
    _uuidToNs[uuid] = ns.toString();
}

rpc::UniqueReply MockRemoteDBServer::runCommand(InstanceID id, const OpMsgRequest& request) {
    checkIfUp(id);
    std::string cmdName = request.getCommandName().toString();

    StatusWith<BSONObj> reply([this, &cmdName] {
        scoped_spinlock lk(_lock);

        uassert(ErrorCodes::IllegalOperation,
                str::stream() << "no reply for command: " << cmdName,
                _cmdMap.count(cmdName));

        return _cmdMap[cmdName]->next();
    }());

    if (_delayMilliSec > 0) {
        mongo::sleepmillis(_delayMilliSec);
    }

    checkIfUp(id);

    {
        scoped_spinlock lk(_lock);
        _cmdCount++;
    }

    // We need to construct a reply message - it will always be read through a view so it
    // doesn't matter whether we use OpMsgReplyBuilder or LegacyReplyBuilder
    auto message = rpc::OpMsgReplyBuilder{}.setCommandReply(reply).done();
    auto replyView = std::make_unique<rpc::OpMsgReply>(&message);
    return rpc::UniqueReply(std::move(message), std::move(replyView));
}

std::unique_ptr<projection_executor::ProjectionExecutor>
MockRemoteDBServer::createProjectionExecutor(const BSONObj& projectionSpec) {
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ProjectionPolicies defaultPolicies;
    auto projection = projection_ast::parseAndAnalyze(expCtx, projectionSpec, defaultPolicies);
    return projection_executor::buildProjectionExecutor(
        expCtx, &projection, defaultPolicies, projection_executor::kDefaultBuilderParams);
}

BSONObj MockRemoteDBServer::project(projection_executor::ProjectionExecutor* projectionExecutor,
                                    const BSONObj& o) {
    if (!projectionExecutor)
        return o.copy();
    Document doc(o);
    auto projectedDoc = projectionExecutor->applyTransformation(doc);
    return projectedDoc.toBson().getOwned();
}

mongo::BSONArray MockRemoteDBServer::findImpl(InstanceID id,
                                              const NamespaceStringOrUUID& nsOrUuid,
                                              BSONObj projection) {
    checkIfUp(id);

    if (_delayMilliSec > 0) {
        mongo::sleepmillis(_delayMilliSec);
    }

    checkIfUp(id);

    std::unique_ptr<projection_executor::ProjectionExecutor> projectionExecutor;
    if (!projection.isEmpty()) {
        projectionExecutor = createProjectionExecutor(projection);
    }
    scoped_spinlock sLock(_lock);
    _queryCount++;

    auto ns = nsOrUuid.uuid() ? _uuidToNs[*nsOrUuid.uuid()] : nsOrUuid.nss()->ns().toString();
    const vector<BSONObj>& coll = _dataMgr[ns];
    BSONArrayBuilder result;
    for (vector<BSONObj>::const_iterator iter = coll.begin(); iter != coll.end(); ++iter) {
        result.append(project(projectionExecutor.get(), *iter));
    }

    return BSONArray(result.obj());
}

mongo::BSONArray MockRemoteDBServer::find(MockRemoteDBServer::InstanceID id,
                                          const FindCommandRequest& findRequest) {
    return findImpl(id, findRequest.getNamespaceOrUUID(), findRequest.getProjection());
}

mongo::ConnectionString::ConnectionType MockRemoteDBServer::type() const {
    return mongo::ConnectionString::ConnectionType::kCustom;
}

size_t MockRemoteDBServer::getCmdCount() const {
    scoped_spinlock sLock(_lock);
    return _cmdCount;
}

size_t MockRemoteDBServer::getQueryCount() const {
    scoped_spinlock sLock(_lock);
    return _queryCount;
}

void MockRemoteDBServer::clearCounters() {
    scoped_spinlock sLock(_lock);
    _cmdCount = 0;
    _queryCount = 0;
}

string MockRemoteDBServer::getServerAddress() const {
    return _hostAndPort.toString();
}

string MockRemoteDBServer::toString() {
    return _hostAndPort.toString();
}

const HostAndPort& MockRemoteDBServer::getServerHostAndPort() const {
    return _hostAndPort;
}

void MockRemoteDBServer::checkIfUp(InstanceID id) const {
    scoped_spinlock sLock(_lock);

    if (!_isRunning || id < _instanceID) {
        throwSocketError(mongo::SocketErrorKind::CLOSED, _hostAndPort.toString());
    }
}
}  // namespace mongo
