/**
 *    Copyright (C) 2012 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include "mongo/db/instance.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/db/wire_version.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {

    class CustomDirectClient: public DBDirectClient {
    public:
        CustomDirectClient(OperationContext* txn) : DBDirectClient(txn) {
            setWireVersions(minWireVersion, maxWireVersion);
        }

        virtual ConnectionString::ConnectionType type() const {
            return ConnectionString::CUSTOM;
        }

        virtual bool recv( Message& m ) {
            // This is tailored to act as a dummy response for write commands.

            BufBuilder bb;
            bb.skip(sizeof(QueryResult));

            BSONObj cmdResult(BSON("ok" << 1));

            bb.appendBuf(cmdResult.objdata(), cmdResult.objsize());

            QueryResult* qr = reinterpret_cast<QueryResult*>(bb.buf());
            bb.decouple();
            qr->setResultFlagsToOk();
            qr->len = bb.len();
            qr->setOperation(opReply);
            qr->cursorId = 0;
            qr->startingFrom = 0;
            qr->nReturned = 1;
            m.setData(qr, true);

            return true;
        }
    };

    class CustomConnectHook : public ConnectionString::ConnectionHook {
    public:
        CustomConnectHook(OperationContext* txn);

        virtual DBClientBase* connect(const ConnectionString& connStr,
                                      std::string& errmsg,
                                      double socketTimeout);

    private:
        OperationContext* _txn;
    };

    /**
     * Fixture for testing complicated operations against a "virtual" config server.
     *
     * Use this if your test requires complex commands and writing to many collections,
     * otherwise a unit test in the mock framework may be a better option.
     */
    class ConfigServerFixture: public mongo::unittest::Test {
    public:

        ConfigServerFixture() : _client(&_txn) {

        }

        /**
         * Returns a client connection to the virtual config server.
         */
        DBClientBase& client() {
            return _client;
        }

        /**
         * Returns a connection std::string to the virtual config server.
         */
        ConnectionString configSvr() const {
            return ConnectionString(std::string("$dummy:10000"));
        }

        /**
         * Clears all data on the server
         */
        void clearServer();

        void clearVersion();
        void clearShards();
        void clearDatabases();
        void clearCollections();
        void clearChunks();
        void clearPings();
        void clearChangelog();

        /**
         * Dumps the contents of the config server to the log.
         */
        void dumpServer();

    protected:

        virtual void setUp();

        virtual void tearDown();

    private:

        OperationContextImpl _txn;
        CustomDirectClient _client;
        CustomConnectHook* _connectHook;
    };

}
