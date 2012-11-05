/*    Copyright 2012 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "mongo/dbtests/mock/mock_remote_db_server.h"

#include "mongo/dbtests/mock/mock_dbclient_connection.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/sock.h"
#include "mongo/util/time_support.h"

using mongo::BSONObj;
using mongo::scoped_spinlock;
using mongo::str::stream;

using std::string;
using std::vector;

namespace mongo_test {
    MockRemoteDBServer::CircularBSONIterator::CircularBSONIterator(
            const vector<BSONObj>& replyVector) {
        for (std::vector<mongo::BSONObj>::const_iterator iter = replyVector.begin();
                iter != replyVector.end(); ++iter) {
            _replyObjs.push_back(iter->copy());
        }

        _iter = _replyObjs.begin();
    }

    BSONObj MockRemoteDBServer::CircularBSONIterator::next() {
        verify(_iter != _replyObjs.end());

        BSONObj reply = _iter->copy();
        ++_iter;

        if (_iter == _replyObjs.end()) {
            _iter = _replyObjs.begin();
        }

        return reply;
    }

    MockRemoteDBServer::MockRemoteDBServer(const string& hostName):
            _isRunning(true),
            _hostName(hostName),
            _delayMilliSec(0),
            _cmdCount(0),
            _queryCount(0),
            _connStrHook(this) {
    }

    MockRemoteDBServer::~MockRemoteDBServer() {
    }

    mongo::ConnectionString::ConnectionHook* MockRemoteDBServer::getConnectionHook() {
        return &_connStrHook;
    }

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
            const mongo::BSONObj& replyObj) {
        vector<BSONObj> replySequence;
        replySequence.push_back(replyObj);
        setCommandReply(cmdName, replySequence);
    }

    void MockRemoteDBServer::setCommandReply(const string& cmdName,
            const vector<BSONObj>& replySequence) {
        scoped_spinlock sLock(_lock);
        _cmdMap[cmdName].reset(new CircularBSONIterator(replySequence));
    }

    bool MockRemoteDBServer::runCommand(MockRemoteDBServer::InstanceID id,
            const string& dbname,
            const BSONObj& cmdObj,
            BSONObj &info,
            int options,
            const mongo::AuthenticationTable* auth) {
        checkIfUp(id);

        // Get the name of the command - copied from _runCommands @ db/dbcommands.cpp
        BSONObj innerCmdObj;
        {
            mongo::BSONElement e = cmdObj.firstElement();
            if (e.type() == mongo::Object && (e.fieldName()[0] == '$'
                    ? mongo::str::equals("query", e.fieldName()+1) :
                            mongo::str::equals("query", e.fieldName()))) {
                innerCmdObj = e.embeddedObject();
            }
            else {
                innerCmdObj = cmdObj;
            }
        }

        string cmdName = innerCmdObj.firstElement().fieldName();
        uassert(16430, stream() << "no reply for cmd: " << cmdName, _cmdMap.count(cmdName) == 1);

        {
            scoped_spinlock sLock(_lock);
            info = _cmdMap[cmdName]->next();
        }

        if (_delayMilliSec > 0) {
            mongo::sleepmillis(_delayMilliSec);
        }

        checkIfUp(id);

        scoped_spinlock sLock(_lock);
        _cmdCount++;
        return info["ok"].trueValue();
    }

    std::auto_ptr<mongo::DBClientCursor> MockRemoteDBServer::query(
            MockRemoteDBServer::InstanceID id,
            const string& ns,
            mongo::Query query,
            int nToReturn,
            int nToSkip,
            const BSONObj* fieldsToReturn,
            int queryOptions,
            int batchSize) {
        checkIfUp(id);

        if (_delayMilliSec > 0) {
            mongo::sleepmillis(_delayMilliSec);
        }

        checkIfUp(id);

        std::auto_ptr<mongo::DBClientCursor> cursor;

        scoped_spinlock sLock(_lock);
        _queryCount++;
        return cursor;
    }

    mongo::ConnectionString::ConnectionType MockRemoteDBServer::type() const {
        return mongo::ConnectionString::CUSTOM;
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
        return _hostName;
    }

    string MockRemoteDBServer::toString() {
        return _hostName;
    }

    void MockRemoteDBServer::checkIfUp(InstanceID id) const {
        scoped_spinlock sLock(_lock);

        if (!_isRunning || id < _instanceID) {
            throw mongo::SocketException(mongo::SocketException::CLOSED, _hostName);
        }
    }

    MockRemoteDBServer::MockDBClientConnStrHook::MockDBClientConnStrHook(
            MockRemoteDBServer* mockServer): _mockServer(mockServer) {
    }

    MockRemoteDBServer::MockDBClientConnStrHook::~MockDBClientConnStrHook() {
    }

    mongo::DBClientBase* MockRemoteDBServer::MockDBClientConnStrHook::connect(
            const mongo::ConnectionString& connString,
            std::string& errmsg,
            double socketTimeout) {
        if (_mockServer->isRunning()) {
            return new MockDBClientConnection(_mockServer);
        }
        else {
            // mimic ConnectionString::connect for MASTER type connection to return NULL
            // if the destination is unreachable.
            return NULL;
        }
    }
}
