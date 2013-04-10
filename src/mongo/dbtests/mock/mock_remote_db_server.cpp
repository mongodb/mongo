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

using std::string;
using std::vector;

namespace mongo {

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

    MockRemoteDBServer::MockRemoteDBServer(const string& hostAndPort):
            _isRunning(true),
            _hostAndPort(hostAndPort),
            _delayMilliSec(0),
            _cmdCount(0),
            _queryCount(0),
            _instanceID(0) {
        insert(IdentityNS, BSON(HostField(hostAndPort)), 0);
    }

    MockRemoteDBServer::~MockRemoteDBServer() {
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

    void MockRemoteDBServer::insert(const string &ns, BSONObj obj, int flags) {
        scoped_spinlock sLock(_lock);

        vector<BSONObj>& mockCollection = _dataMgr[ns];
        mockCollection.push_back(obj.copy());
    }

    void MockRemoteDBServer::remove(const string& ns, Query query, int flags) {
        scoped_spinlock sLock(_lock);
        if (_dataMgr.count(ns) == 0) {
            return;
        }

        _dataMgr.erase(ns);
    }

    bool MockRemoteDBServer::runCommand(MockRemoteDBServer::InstanceID id,
            const string& dbname,
            const BSONObj& cmdObj,
            BSONObj &info,
            int options) {
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
        uassert(16430, str::stream() << "no reply for cmd: " << cmdName,
                _cmdMap.count(cmdName) == 1);

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

    mongo::BSONArray MockRemoteDBServer::query(
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

        scoped_spinlock sLock(_lock);
        _queryCount++;

        const vector<BSONObj>& coll = _dataMgr[ns];
        BSONArrayBuilder result;
        for (vector<BSONObj>::const_iterator iter = coll.begin(); iter != coll.end(); ++ iter) {
            result.append(iter->copy());
        }

        return BSONArray(result.obj());
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
        return _hostAndPort;
    }

    string MockRemoteDBServer::toString() {
        return _hostAndPort;
    }

    void MockRemoteDBServer::checkIfUp(InstanceID id) const {
        scoped_spinlock sLock(_lock);

        if (!_isRunning || id < _instanceID) {
            throw mongo::SocketException(mongo::SocketException::CLOSED, _hostAndPort);
        }
    }
}
