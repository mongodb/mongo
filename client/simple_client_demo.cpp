/* simple_client_demo.cpp

   See also : http://www.mongodb.org/pages/viewpage.action?pageId=133415

   How to build and run:

   (1) Using the mongoclient:
    g++ simple_client_demo.cpp -lmongoclient -lboost_thread-mt -lboost_filesystem -lboost_program_options
    ./a.out

   (2) using client_lib.cpp:
    g++ -I .. simple_client_demo.cpp mongo_client_lib.cpp -lboost_thread-mt -lboost_filesystem
    ./a.out
*/

#include <iostream>
#include "dbclient.h" // the mongo c++ driver

using namespace std;
using namespace mongo;
using namespace bson;

int main() {
    try {
        cout << "connecting to localhost..." << endl;
        DBClientConnection c;
        c.connect("localhost");
        cout << "connected ok" << endl;
        unsigned long long count = c.count("test.foo");
        cout << "count of exiting documents in collection test.foo : " << count << endl;

        bo o = BSON( "hello" << "world" );
        c.insert("test.foo", o);

        string e = c.getLastError();
        if( !e.empty() ) { 
            cout << "insert #1 failed: " << e << endl;
        }

        // make an index with a unique key constraint
        c.ensureIndex("test.foo", BSON("hello"<<1), /*unique*/true);

        c.insert("test.foo", o); // will cause a dup key error on "hello" field
        cout << "we expect a dup key error here:" << endl;
        cout << "  " << c.getLastErrorDetailed().toString() << endl;
    } 
    catch(DBException& e) { 
        cout << "caught DBException " << e.toString() << endl;
        return 1;
    }

    return 0;
}

