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
    cout << "connecting to localhost..." << endl;
    DBClientConnection c;
    c.connect("localhost");
    cout << "connected ok" << endl;
    unsigned long long count = c.count("test.foo");
    cout << "count of exiting documents in collection test.foo : " << count << endl;

    bo o = BSON( "hello" << "world" );
    c.insert("test.foo", o);

    return 0;
}

