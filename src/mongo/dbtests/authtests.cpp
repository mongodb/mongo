// authtests.cpp : unit tests relating to authentication.
//

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

#include "../db/security.h"
#include "dbtests.h"

namespace AuthTests {

    /** Simple test for AuthenticationInfo::setTemporaryAuthorization. */
    class TempAuth {
    public:
        void run() {
            bool authEnabled = mongo::noauth;
            mongo::noauth = false; // Enable authentication.
            AuthenticationInfo ai;

            ASSERT( ! ai.isAuthorized( "test" ) );
            ASSERT( ! ai.isAuthorized( "admin" ) );
            ASSERT( ! ai.isAuthorizedReads( "test" ) );
            ASSERT( ! ai.isAuthorizedReads( "admin" ) );

            ai.authorizeReadOnly( "admin", "adminRO" );
            ASSERT( ! ai.isAuthorized( "test" ) );
            ASSERT( ! ai.isAuthorized( "admin" ) );
            ASSERT( ai.isAuthorizedReads( "test" ) );
            ASSERT( ai.isAuthorizedReads( "admin" ) );

            BSONObj input = BSON(
                                 "admin" << BSON( "adminRO" << 1 ) <<
                                 "test" << BSON( "testRW" << 2 )
                                 );
            ai.setTemporaryAuthorization( input );
            ASSERT( ai.isAuthorized( "test" ) );
            ASSERT( ! ai.isAuthorized( "admin" ) );
            ASSERT( ! ai.isAuthorized( "test2" ) );
            ASSERT( ai.isAuthorizedReads( "test" ) );
            ASSERT( ai.isAuthorizedReads( "admin" ) );
            ASSERT( ai.isAuthorizedReads( "test2" ) );

            ai.clearTemporaryAuthorization();
            ASSERT( ! ai.isAuthorized( "test" ) );
            ASSERT( ! ai.isAuthorized( "admin" ) );
            ASSERT( ai.isAuthorizedReads( "test" ) );
            ASSERT( ai.isAuthorizedReads( "admin" ) );
            mongo::noauth = authEnabled; // Restore authentication.
        }
    };

    /** Simple test for AuthenticationTable::toBSON. */
    class ToBSON {
    public:
        void run() {
            AuthenticationTable at;
            at.addAuth("admin", "adminUser", Auth::WRITE);
            at.addAuth("test", "testUser", Auth::WRITE);
            at.addAuth("local", "localUser", Auth::READ);

            BSONObj expected = BSON(
                                 "admin" << BSON( "adminUser" << 2 ) <<
                                 "test" << BSON( "testUser" << 2 ) <<
                                 "local" << BSON( "localUser" << 1 )
                                 );

            ASSERT( bson2set(expected) == bson2set(at.toBSON()) );
        }
    };

    class All : public Suite {
    public:
        All() : Suite( "auth" ) {
        }
        void setupTests() {
            add<TempAuth>();
            add<ToBSON>();
        }
    } myall;

} // namespace AuthTests
