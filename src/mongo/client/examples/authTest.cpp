// authTest.cpp

/*    Copyright 2009 10gen Inc.
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

#include <boost/scoped_ptr.hpp>
#include <iostream>
#include <cstdlib>
#include <string>
#include "mongo/client/dbclient.h"

using namespace mongo;

int main( int argc, const char **argv ) {

    const char *port = "27017";
    if ( argc != 1 ) {
        if ( argc != 3 ) {
            std::cout << "need to pass port as second param" << endl;
            return EXIT_FAILURE;
        }
        port = argv[ 2 ];
    }

    std::string errmsg;
    ConnectionString cs = ConnectionString::parse(string("127.0.0.1:") + port, errmsg);
    if (!cs.isValid()) {
        cout << "error parsing url: " << errmsg << endl;
        return EXIT_FAILURE;
    }

    boost::scoped_ptr<DBClientBase> conn(cs.connect(errmsg));
    if (!conn) {
        cout << "couldn't connect: " << errmsg << endl;
        return EXIT_FAILURE;
    }

    BSONObj ret;
    // clean up old data from any previous tests
    conn->runCommand( "test", BSON("removeUsersFromDatabase" << 1), ret );

    conn->runCommand( "test",
                      BSON( "createUser" << "eliot" <<
                            "pwd" << "bar" <<
                            "roles" << BSON_ARRAY("readWrite")),
                      ret);

    errmsg.clear();
    conn->auth(BSON("user" << "eliot" <<
                    "userSource" << "test" <<
                    "pwd" << "bar" <<
                    "mechanism" << "MONGODB-CR"));

    try {
        conn->auth(BSON("user" << "eliot" <<
                        "userSource" << "test" <<
                        "pwd" << "bars" << // incorrect password
                        "mechanism" << "MONGODB-CR"));
        // Shouldn't get here.
        cout << "Authentication with invalid password should have failed but didn't" << endl;
        return EXIT_FAILURE;
    } catch (const DBException& e) {
        // expected
    }
    return EXIT_SUCCESS;
}
