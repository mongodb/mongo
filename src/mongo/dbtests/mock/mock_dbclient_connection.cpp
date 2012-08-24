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

#include "mongo/util/net/sock.h"

using mongo::BSONObj;

using std::string;
using std::vector;

namespace mongo_test {
    MockDBClientConnection::MockDBClientConnection(MockRemoteDBServer* remoteServer):
            _remoteServerInstanceID(remoteServer->getInstanceID()),
            _remoteServer(remoteServer),
            _isFailed(false) {
    }

    MockDBClientConnection::~MockDBClientConnection() {
    }

    bool MockDBClientConnection::runCommand(const string& dbname, const BSONObj& cmdObj,
            BSONObj &info, int options, const mongo::AuthenticationTable* auth) {
        try {
            return _remoteServer->runCommand(_remoteServerInstanceID, dbname, cmdObj,
                    info, options, auth);
        }
        catch (const mongo::SocketException& exp) {
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
        try {
            return _remoteServer->query(_remoteServerInstanceID, ns, query, nToReturn,
                    nToSkip, fieldsToReturn, queryOptions, batchSize);
        }
        catch (const mongo::SocketException& exp) {
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
}
