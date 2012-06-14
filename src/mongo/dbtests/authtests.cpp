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

            {
                AuthenticationInfo::TemporaryAuthReleaser authRelease( &ai );
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

                {
                    // This shouldn't actually clear the temporary auth when it goes out of scope
                    // because there is already temporary auth set at this point.
                    AuthenticationInfo::TemporaryAuthReleaser authRelease( &ai );
                }

                // Auth should be the same as before the second TemporaryAuthReleaser
                ASSERT( ai.isAuthorized( "test" ) );
                ASSERT( ! ai.isAuthorized( "admin" ) );
                ASSERT( ! ai.isAuthorized( "test2" ) );
                ASSERT( ai.isAuthorizedReads( "test" ) );
                ASSERT( ai.isAuthorizedReads( "admin" ) );
                ASSERT( ai.isAuthorizedReads( "test2" ) );
            }

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

    /** Simple test for AuthenticationTable::copyCommandObjAddingAuth. */
    class AddAuth {
    public:
        void run() {
            AuthenticationTable at;
            at.addAuth("test", "testUser", Auth::WRITE);

            BSONObj cmd = BSON( "commandName" << "commandValue" );
            BSONObj cmdWithAuth = at.copyCommandObjAddingAuth( cmd );
            BSONObj expected = BSON(
                                    "commandName" << "commandValue" <<
                                    "$auth" << BSON( "test" << BSON( "testUser" << 2 ) )
                                 );

            // Make sure a malicious user can't set their own $auth
            BSONObj bogusAuthCmd = BSON(
                                        "commandName" << "commandValue" <<
                                        "$auth" << BSON( "admin" << BSON( "adminUser" << 2 ) )
                                        );
            BSONObj bogusAuthCmdWithAuth = at.copyCommandObjAddingAuth( bogusAuthCmd );
            log() << "bogusAuthCmd: " << bogusAuthCmd << endl;
            log() << "bogusAuthCmdWithAuth: " << bogusAuthCmdWithAuth << endl;
            ASSERT( bson2set(expected) == bson2set(bogusAuthCmdWithAuth) );
        }
    };

    class All : public Suite {
    public:
        All() : Suite( "auth" ) {
        }
        void setupTests() {
            add<TempAuth>();
            add<ToBSON>();
            add<AddAuth>();
        }
    } myall;

} // namespace AuthTests
