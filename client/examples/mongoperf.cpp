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

int dummy;
LogFile *lf = 0;
MemoryMappedFile *mmfFile;
char *mmf = 0;
bo options;
unsigned long long len; // file len
const unsigned PG = 4096;
unsigned nThreadsRunning = 0;

// as this is incremented A LOT, at some point this becomes a bottleneck if very high ops/second (in cache) things are happening.
AtomicUInt iops;

SimpleMutex m("mperf");

char* round(char* x) {
    size_t f = (size_t) x;
    char *p = (char *) ((f+PG-1)/PG*PG);
    return p;
}

struct Aligned {
    char x[8192];
    char* addr() { return round(x); }
}; 

unsigned long long rrand() { 
    // RAND_MAX is very small on windows
    return (static_cast<unsigned long long>(rand()) << 15) ^ rand();
}

void workerThread() {
    bool r = options["r"].trueValue();
    bool w = options["w"].trueValue();
    long long su = options["sleepMicros"].numberLong();
    Aligned a;
    while( 1 ) { 
        unsigned long long rofs = (rrand() * PG) % len;
        unsigned long long wofs = (rrand() * PG) % len;
        if( mmf ) { 
            if( r ) {
                dummy += mmf[rofs];
                iops++;
            }
            if( w ) {
                mmf[wofs] = 3;
                iops++;
            }
        }
        else {
            if( r ) {
                lf->readAt((unsigned) rofs, a.addr(), PG);
                iops++;
            }
            if( w ) {
                lf->writeAt((unsigned) wofs, a.addr(), PG);
                iops++;
            }
        }
        long long micros = su / nThreadsRunning;
        if( micros ) {
            sleepmicros(micros);
        }
    }
}

void go() {
    assert( options["r"].trueValue() || options["w"].trueValue() );
    MemoryMappedFile f;
    cout << "creating test file size:";
    len = options["fileSizeMB"].numberLong();
    if( len == 0 ) len = 1;
    cout << len << "MB ..." << endl;

    if( len > 2000 && !options["mmf"].trueValue() ) { 
        // todo make tests use 64 bit offsets in their i/o -- i.e. adjust LogFile::writeAt and such
        cout << "\nsizes > 2GB not yet supported with mmf:false" << endl; 
        return;
    }
    len *= 1024 * 1024;
    const char *fname = "./mongoperf__testfile__tmp";
    try {
        boost::filesystem::remove(fname);
    }
    catch(...) { 
        cout << "error deleting file " << fname << endl;
        return;
    }
    lf = new LogFile(fname,true);
    const unsigned sz = 1024 * 1024 * 32; // needs to be big as we are using synchronousAppend.  if we used a regular MongoFile it wouldn't have to be
    char *buf = (char*) malloc(sz+4096);
    const char *p = round(buf);
    for( unsigned long long i = 0; i < len; i += sz ) { 
        lf->synchronousAppend(p, sz);
        if( i % (1024ULL*1024*1024) == 0 && i ) {
            cout << i / (1024ULL*1024*1024) << "GB..." << endl;
        }
    }
    BSONObj& o = options;

    if( o["mmf"].trueValue() ) { 
        delete lf;
        lf = 0;
        mmfFile = new MemoryMappedFile();
        mmf = (char *) mmfFile->map(fname);
        assert( mmf );
    }

    cout << "testing..."<< endl;

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
                    boost::thread w(workerThread);
                }
                cout << "new thread, total running : " << nthr << endl;
                d *= 2;
            }
        }
        sleepsecs(4);
        unsigned long long w = iops.get();
        iops.zero();
        w /= 4; // 4 secs
        cout << w << " ops/sec ";
        if( mmf == 0 ) 
            // only writing 4 bytes with mmf so we don't say this
            cout << (w * PG / 1024 / 1024) << " MB/sec";
        cout << endl;
    }
}

int main(int argc, char *argv[]) {

    try {
        cout << "mongoperf" << endl;

        if( argc > 1 ) { 
cout <<

"\n"
"usage:\n"
"\n"
"  mongoperf < myjsonconfigfile\n"
"\n"
"  {\n"
"    nThreads:<n>,     // number of threads\n"
"    fileSizeMB:<n>,   // test file size\n"
"    sleepMicros:<n>,  // pause for sleepMicros/nThreads between each operation\n"
"    mmf:<bool>,       // if true do i/o's via memory mapped files\n"
"    r:<bool>,         // do reads\n"
"    w:<bool>          // do writes\n"
"  }\n"
"\n"
"most fields are optional.\n"
"non-mmf io is direct io (no caching). use a large file size to test making the heads\n"
"  move significantly and to avoid i/o coalescing\n"
"mmf io uses caching (the file system cache).\n"
"\n"

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
        try { 
            options = fromjson(s);
        }
        catch(...) { 
            cout << s << endl;
            cout << "couldn't parse json options" << endl;
            return -1;
        }
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

