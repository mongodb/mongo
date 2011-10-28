/* 
   How to build and run:

    g++ -o mongoperf -I .. mongoperf.cpp mongo_client_lib.cpp -lboost_thread-mt -lboost_filesystem
*/

#include <iostream>
#include "../dbclient.h" // the mongo c++ driver

using namespace std;
using namespace mongo;
using namespace bson;

void writer() { 
}

void go(bo o) {
    int w = o["w"].Int();
    for( int i = 0; i < w; i++ ) { 
        boost::thread w(writer);
    }
}

int main(int argc, char *argv[]) {

    try {
        cout << "mongoperf" << endl;

        if( argc > 1 ) { 
            cout << "help\n" 
                "  pipe a json object to mongoperf's stdin. this object specifies the options\n"
                "  for the performance test\n"
                "  options:\n"
                "    w:<n> number of write threads"
                << endl;
            return 0;
        }

        cout << "use -h for help" << endl;

        char input[1024];
        memset(input, 0, sizeof(input));
        cin.read(input, 1000);
        if( *input == 0 ) { 
            cout << "error no options found on stdin for mongoperf" << endl;
            return 2;
        }

        string s = input;
        str::stripTrailing(s, "\n\r\0x1a");
        bo options = fromjson(s);
        cout << "options:\n" << options.toString() << endl;

        go(options);
#if 0
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
#endif
    } 
    catch(DBException& e) { 
        cout << "caught DBException " << e.toString() << endl;
        return 1;
    }

    return 0;
}

