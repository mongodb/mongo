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

#pragma once

#include "mongo/client/dbclientinterface.h"
#include "mongo/db/dbmessage.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {

class OperationContext;

/**
 * Embedded calls to the local server using the DBClientBase API without going over the network.
 *
 * Caller does not need to lock, that is handled within.
 *
 * All operations are performed within the scope of a passed-in OperationContext (except when
 * using the deprecated constructor). You must ensure that the OperationContext is valid when
 * calling into any function. If you ever need to change the OperationContext, that can be done
 * without the overhead of creating a new DBDirectClient by calling setOpCtx(), after which all
 * operations will use the new OperationContext.
 */
class DBDirectClient : public DBClientBase {
public:
    static const HostAndPort dummyHost;

    DBDirectClient(OperationContext* txn);

    using DBClientBase::query;

    // XXX: is this valid or useful?
    void setOpCtx(OperationContext* txn);

    virtual std::unique_ptr<DBClientCursor> query(const std::string& ns,
                                                  Query query,
                                                  int nToReturn = 0,
                                                  int nToSkip = 0,
                                                  const BSONObj* fieldsToReturn = 0,
                                                  int queryOptions = 0,
                                                  int batchSize = 0);

    virtual bool isFailed() const;

    virtual bool isStillConnected();

    virtual std::string toString() const;

    virtual std::string getServerAddress() const;

    virtual bool call(Message& toSend,
                      Message& response,
                      bool assertOk = true,
                      std::string* actualServer = 0);

    virtual void say(Message& toSend, bool isRetry = false, std::string* actualServer = 0);

    virtual unsigned long long count(const std::string& ns,
                                     const BSONObj& query = BSONObj(),
                                     int options = 0,
                                     int limit = 0,
                                     int skip = 0);

    virtual ConnectionString::ConnectionType type() const;

    double getSoTimeout() const;

    virtual bool lazySupported() const;

    virtual QueryOptions _lookupAvailableOptions();

    int getMinWireVersion() final;
    int getMaxWireVersion() final;

private:
    OperationContext* _txn;
};

}  // namespace mongo
