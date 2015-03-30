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

#include "mongo/client/dbclientinterface.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/db/wire_version.h"
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
            bb.skip(sizeof(QueryResult::Value));

            BSONObj cmdResult(BSON("ok" << 1));

            bb.appendBuf(cmdResult.objdata(), cmdResult.objsize());

            QueryResult::View qr = bb.buf();
            bb.decouple();
            qr.setResultFlagsToOk();
            qr.msgdata().setLen(bb.len());
            qr.msgdata().setOperation(opReply);
            qr.setCursorId(0);
            qr.setStartingFrom(0);
            qr.setNReturned(1);
            m.setData(qr.view2ptr(), true);

            return true;
        }
    };

    class CustomConnectHook : public ConnectionString::ConnectionHook {
    public:
        CustomConnectHook(OperationContext* txn) : _txn(txn) { }

        virtual DBClientBase* connect(const ConnectionString& connStr,
                                      std::string& errmsg,
                                      double socketTimeout)
        {
            // Note - must be new, since it gets owned elsewhere
            return new CustomDirectClient(_txn);
        }

    private:
        OperationContext* const _txn;
    };

    /**
     * Fixture for testing complicated operations against a "virtual" config server.
     *
     * Use this if your test requires complex commands and writing to many collections,
     * otherwise a unit test in the mock framework may be a better option.
     */
    class ConfigServerFixture: public mongo::unittest::Test {
    public:

        ConfigServerFixture();

        /**
         * Returns a connection std::string to the virtual config server.
         */
        ConnectionString configSvr() const {
            return ConnectionString(HostAndPort("$dummy:10000"));
        }

        /**
         * Clears all data on the server
         */
        void clearServer();

        void clearVersion();

        /**
         * Dumps the contents of the config server to the log.
         */
        void dumpServer();

    protected:

        virtual void setUp();

        virtual void tearDown();


        OperationContextImpl _txn;
        CustomDirectClient _client;
        CustomConnectHook* _connectHook;
    };

}
