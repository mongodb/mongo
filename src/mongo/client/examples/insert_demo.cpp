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

/* 
   C++ client program which inserts documents in a MongoDB database.

   How to build and run:

   Using mongo_client_lib.cpp:
    g++ -I .. -I ../.. insert_demo.cpp ../mongo_client_lib.cpp -lboost_thread-mt -lboost_filesystem
    ./a.out
*/

#include <iostream>
#include "mongo/client/dbclient.h" // the mongo c++ driver

using namespace std;
using namespace mongo;
using namespace bson;

int main() {
    try {
        cout << "connecting to localhost..." << endl;
        DBClientConnection c;
        c.connect("localhost");
        cout << "connected ok" << endl;

        bo o = BSON( "hello" << "world" );

	cout << "inserting..." << endl;

	time_t start = time(0);
	for( unsigned i = 0; i < 1000000; i++ ) {
	  c.insert("test.foo", o);
	}

	// wait until all operations applied
	cout << "getlasterror returns: \"" << c.getLastError() << '"' << endl;

	time_t done = time(0);
	time_t dt = done-start;
	cout << dt << " seconds " << 1000000/dt << " per second" << endl;
    } 
    catch(DBException& e) { 
        cout << "caught DBException " << e.toString() << endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
