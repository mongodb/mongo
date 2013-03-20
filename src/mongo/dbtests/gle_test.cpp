/**
 *    Copyright (C) 2013 10gen Inc.
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

#include "mongo/dbtests/dbtests.h"
#include "mongo/util/assert_util.h"

using mongo::MsgAssertionException;

/**
 * Test getLastError client handling
 */
namespace {
    DBDirectClient _client;
    static const char* const _ns = "unittests.gle";

    /**
     * Verify that when the command fails we get back an error message.
     */
    class GetLastErrorCommandFailure {
    public:
        void run() {
            _client.insert(_ns, BSON( "test" << "test"));
            // Cannot mix fsync + j, will make command fail
            string gleString = _client.getLastError(true, true, 10, 10);
            ASSERT_NOT_EQUALS(gleString, "");
        }
    };

    /**
     * Verify that the write succeeds
     */
    class GetLastErrorClean {
    public:
        void run() {
            _client.insert(_ns, BSON( "test" << "test"));
            // Make sure there was no error
            string gleString = _client.getLastError();
            ASSERT_EQUALS(gleString, "");
        }
    };

    /**
     * Verify that the write succeed first, then error on dup
     */
    class GetLastErrorFromDup {
    public:
        void run() {
            _client.insert(_ns, BSON( "_id" << 1));
            // Make sure there was no error
            string gleString = _client.getLastError();
            ASSERT_EQUALS(gleString, "");

            //insert dup
            _client.insert(_ns, BSON( "_id" << 1));
            // Make sure there was an error
            gleString = _client.getLastError();
            ASSERT_NOT_EQUALS(gleString, "");
        }
    };

    class All : public Suite {
    public:
        All() : Suite( "gle" ) {
        }

        void setupTests() {
            add< GetLastErrorClean >();
            add< GetLastErrorCommandFailure >();
            add< GetLastErrorFromDup >();
        }
    } myall;
}
