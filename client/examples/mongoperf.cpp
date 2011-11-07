/* 
   How to build and run:

   (out of date) : g++ -o mongoperf -I .. mongoperf.cpp mongo_client_lib.cpp -lboost_thread-mt -lboost_filesystem
*/

#include <iostream>
#include "../dbclient.h" // the mongo c++ driver
#include "../../util/mmap.h"
#include <assert.h>
#include "../../util/logfile.h"
#include "../../util/time_support.h"
#include "../../bson/util/atomic_int.h"

using namespace std;
using namespace mongo;
using namespace bson;

LogFile *lf = 0;

//MemoryMappedFile m;
bo options;
unsigned long long len; // file len
const unsigned PG = 4096;
unsigned nThreadsRunning = 0;
char *mmf = 0;
AtomicUInt writes;

void writer() {
    long long su = options["sleepMicros"].numberLong();
    char abuf[PG];
    while( 1 ) { 
        int x = rand();
        unsigned long long ofs = (x * PG) % len;
        lf->writeAt((unsigned) ofs, abuf, PG);
        writes++;
        long long micros = su / nThreadsRunning;
        if( micros ) {
            sleepmicros(micros);
        }
    }
}

void go() {
    MemoryMappedFile f;
    cout << "create test file" << endl;
    len = options["fileSizeMB"].numberLong();
    if( len == 0 ) len = 1;
    cout << "test fileSizeMB : " << len << endl;
    len *= 1024 * 1024;
    const char *fname = "mongoperf__testfile__tmp";
    lf = new LogFile(fname);
    const unsigned sz = 1024 * 256;
    char buf[sz];
    for( unsigned i = 0; i < len; i+= sz ) { 
        lf->synchronousAppend(buf, sz);
    }

    //void *p = m.create("mongoperf__testfile__tmp", len * 1024 * 1024, true);
    //assert(p);

    if( o["mmf"].trueValue() ) { 
        mmf = f.map(fname);
        assert( mmf );
    }

    cout << "testing..."<< endl;

    BSONObj& o = options;
    unsigned wthr = (unsigned) o["nThreads"].Int();
    if( wthr < 1 ) { 
        cout << "bad threads field value" << endl;
        return;
    }
    unsigned i = 0;
    unsigned d = 1;
    unsigned &nthr = nThreadsRunning;
    while( 1 ) {
        if( i++ % 4 == 0 ) {
            if( nthr < wthr ) {
                while( nthr < wthr && nthr < d ) {
                    nthr++;
                    boost::thread w(writer);
                }
                cout << "new thread, total running : " << nthr << endl;
                d *= 2;
            }
        }
        sleepsecs(4);
        unsigned long long w = writes.get();
        writes.zero();
        w /= 4;
        cout << w << " ops/sec " << (w * PG / 1024 / 1024) << " MB/sec" << endl;
    }
}

int main(int argc, char *argv[]) {

    try {
        cout << "mongoperf" << endl;

        if( argc > 1 ) { 
            cout <<
                "  usage:\n\n"
                "    mongoperf < myjsonconfigfile\n\n"
                "  json config doc fields:\n"
                "    nThreads:<n> number of threads\n"
                "    fileSizeMB:<n> test file size. if the file is small the heads will not move much\n"
                "      thus making the test not informative.\n"
                "    sleepMicros:<n> pause for sleepMicros/#threadsrunning between each operation\n"
                "    mmf:true do i/o's via memory mapped files\n"
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
        options = fromjson(s);
        cout << "options:\n" << options.toString() << endl;

        go();
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

