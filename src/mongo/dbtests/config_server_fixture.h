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
 */

#pragma once

#include "mongo/db/instance.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {

    class CustomDirectClient: public DBDirectClient {
    public:
        virtual ConnectionString::ConnectionType type() const {
            return ConnectionString::CUSTOM;
        }
    };

    class CustomConnectHook: public ConnectionString::ConnectionHook {
    public:
        virtual DBClientBase* connect(const ConnectionString& connStr,
                                      string& errmsg,
                                      double socketTimeout)
        {
            // Note - must be new, since it gets owned elsewhere
            return new CustomDirectClient();
        }
    };

    /**
     * Fixture for testing complicated operations against a "virtual" config server.
     *
     * Use this if your test requires complex commands and writing to many collections,
     * otherwise a unit test in the mock framework may be a better option.
     */
    class ConfigServerFixture: public mongo::unittest::Test {
    public:

        /**
         * Returns a client connection to the virtual config server.
         */
        DBClientBase& client() {
            return _client;
        }

        /**
         * Returns a connection string to the virtual config server.
         */
        ConnectionString configSvr() const {
            return ConnectionString(string("$dummy:10000"));
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

        CustomConnectHook* _connectHook;
        CustomDirectClient _client;
    };

}
