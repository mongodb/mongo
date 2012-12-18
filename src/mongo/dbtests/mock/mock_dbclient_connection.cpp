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

#include "mongo/dbtests/mock/mock_dbclient_connection.h"

#include "mongo/dbtests/mock/mock_dbclient_cursor.h"
#include "mongo/util/net/sock.h"
#include "mongo/util/time_support.h"

using mongo::BSONObj;

using std::string;
using std::vector;

namespace mongo {
    MockDBClientConnection::MockDBClientConnection(MockRemoteDBServer* remoteServer,
            bool autoReconnect):
            _remoteServerInstanceID(remoteServer->getInstanceID()),
            _remoteServer(remoteServer),
            _isFailed(false),
            _sockCreationTime(mongo::curTimeMicros64()),
            _autoReconnect(autoReconnect) {
    }

    MockDBClientConnection::~MockDBClientConnection() {
    }

    bool MockDBClientConnection::connect(const char* hostName, std::string& errmsg) {
        if (_remoteServer->isRunning()) {
            _remoteServerInstanceID = _remoteServer->getInstanceID();
            return true;
        }

        errmsg.assign("cannot connect to " + _remoteServer->getServerAddress());
        return false;
    }

    bool MockDBClientConnection::runCommand(const string& dbname, const BSONObj& cmdObj,
            BSONObj &info, int options) {
        checkConnection();

        try {
            return _remoteServer->runCommand(_remoteServerInstanceID, dbname, cmdObj,
                    info, options);
        }
        catch (const mongo::SocketException&) {
            _isFailed = true;
            throw;
        }

        return false;
    }

    std::auto_ptr<mongo::DBClientCursor> MockDBClientConnection::query(const string& ns,
            mongo::Query query,
            int nToReturn,
            int nToSkip,
            const BSONObj* fieldsToReturn,
            int queryOptions,
            int batchSize) {
        checkConnection();

        try {
            mongo::BSONArray result(_remoteServer->query(_remoteServerInstanceID, ns, query,
                    nToReturn, nToSkip, fieldsToReturn, queryOptions, batchSize));

            std::auto_ptr<mongo::DBClientCursor> cursor;
            cursor.reset(new MockDBClientCursor(this, result));
            return cursor;
        }
        catch (const mongo::SocketException&) {
            _isFailed = true;
            throw;
        }

        std::auto_ptr<mongo::DBClientCursor> nullPtr;
        return nullPtr;
    }

    mongo::ConnectionString::ConnectionType MockDBClientConnection::type() const {
        return mongo::ConnectionString::CUSTOM;
    }

    bool MockDBClientConnection::isFailed() const {
        return _isFailed;
    }

    string MockDBClientConnection::getServerAddress() const {
        return _remoteServer->getServerAddress();
    }

    string MockDBClientConnection::toString() {
        return _remoteServer->toString();
    }

    unsigned long long MockDBClientConnection::query(boost::function<void(const BSONObj&)> f,
                    const string& ns,
                    mongo::Query query,
                    const BSONObj* fieldsToReturn,
                    int queryOptions) {
        verify(false);
        return 0;
    }

    unsigned long long MockDBClientConnection::query(boost::function<void(
            mongo::DBClientCursorBatchIterator&)> f,
            const std::string& ns,
            mongo::Query query,
            const mongo::BSONObj* fieldsToReturn,
            int queryOptions) {
        verify(false);
        return 0;
    }

    uint64_t MockDBClientConnection::getSockCreationMicroSec() const {
        return _sockCreationTime;
    }

    void MockDBClientConnection::insert(const string &ns, BSONObj obj, int flags) {
        _remoteServer->insert(ns, obj, flags);
    }

    void MockDBClientConnection::insert(const string &ns,
            const vector<BSONObj>& objList,
            int flags) {
        for (vector<BSONObj>::const_iterator iter = objList.begin();
                iter != objList.end(); ++iter) {
            insert(ns, *iter, flags);
        }
    }

    void MockDBClientConnection::remove(const string& ns, Query query, bool justOne) {
        remove(ns, query, (justOne ? RemoveOption_JustOne : 0));
    }

    void MockDBClientConnection::remove(const string& ns, Query query, int flags) {
        _remoteServer->remove(ns, query, flags);
    }

    void MockDBClientConnection::killCursor(long long cursorID) {
        verify(false); // unimplemented
    }

    bool MockDBClientConnection::callRead(mongo::Message& toSend , mongo::Message& response) {
        verify(false); // unimplemented
        return false;
    }

    bool MockDBClientConnection::call(mongo::Message& toSend,
            mongo::Message& response,
            bool assertOk,
            string* actualServer)  {
        verify(false); // unimplemented
        return false;
    }

    void MockDBClientConnection::say(mongo::Message& toSend, bool isRetry, string* actualServer) {
        verify(false); // unimplemented
    }

    void MockDBClientConnection::sayPiggyBack(mongo::Message& toSend) {
        verify(false); // unimplemented
    }

    bool MockDBClientConnection::lazySupported() const {
        verify(false); // unimplemented
        return false;
    }

    double MockDBClientConnection::getSoTimeout() const {
        return 0;
    }

    void MockDBClientConnection::checkConnection() {
        if (_isFailed && _autoReconnect) {
            _remoteServerInstanceID = _remoteServer->getInstanceID();
        }
    }
}
