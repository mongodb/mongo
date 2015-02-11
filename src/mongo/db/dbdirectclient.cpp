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

#include "mongo/db/dbdirectclient.h"

#include "mongo/db/client.h"
#include "mongo/db/instance.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/log.h"

namespace mongo {

    using std::auto_ptr;
    using std::endl;
    using std::string;

    // Called from scripting/engine.cpp and scripting/v8_db.cpp.
    DBClientBase* createDirectClient(OperationContext* txn) {
        return new DBDirectClient(txn);
    }

    namespace {

        class GodScope {
            MONGO_DISALLOW_COPYING(GodScope);
        public:
            GodScope(OperationContext* txn) : _txn(txn) {
                _prev = _txn->getClient()->setGod(true);
            }

            ~GodScope() {
                _txn->getClient()->setGod(_prev);
            }

        private:
            bool _prev;
            OperationContext* _txn;
        };

    }  // namespace


    DBDirectClient::DBDirectClient(OperationContext* txn)  : _txn(txn) { }

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
        return "localhost"; // TODO: should this have the port?
    }

    void DBDirectClient::sayPiggyBack(Message& toSend) {
        // don't need to piggy back when connected locally
        return say(toSend);
    }

    bool DBDirectClient::callRead(Message& toSend, Message& response) {
        return call(toSend, response);
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

    bool DBDirectClient::call(Message& toSend,
                              Message& response,
                              bool assertOk,
                              string* actualServer) {

        GodScope gs(_txn);
        if (lastError._get()) {
            lastError.startRequest(toSend, lastError._get());
        }

        DbResponse dbResponse;
        assembleResponse(_txn, toSend, dbResponse, dummyHost, true);
        verify(dbResponse.response);

        // can get rid of this if we make response handling smarter
        dbResponse.response->concat();
        response = *dbResponse.response;

        return true;
    }

    void DBDirectClient::say(Message& toSend, bool isRetry, string* actualServer) {
        GodScope gs(_txn);
        if (lastError._get()) {
            lastError.startRequest(toSend, lastError._get());
        }

        DbResponse dbResponse;
        assembleResponse(_txn, toSend, dbResponse, dummyHost, true);
    }

    auto_ptr<DBClientCursor> DBDirectClient::query(const string& ns,
                                                   Query query,
                                                   int nToReturn,
                                                   int nToSkip,
                                                   const BSONObj* fieldsToReturn,
                                                   int queryOptions,
                                                   int batchSize) {

        return DBClientBase::query(ns,
                                   query,
                                   nToReturn,
                                   nToSkip,
                                   fieldsToReturn,
                                   queryOptions,
                                   batchSize);
    }

    void DBDirectClient::killCursor(long long id) {
        // The killCursor command on the DB client is only used by sharding,
        // so no need to have it for MongoD.
        verify(!"killCursor should not be used in MongoD");
    }

    const HostAndPort DBDirectClient::dummyHost("0.0.0.0", 0);

}  // namespace mongo
