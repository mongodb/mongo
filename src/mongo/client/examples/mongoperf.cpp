/* 
   How to build and run:

   scons mongoperf
   ./mongoperf -h
*/


// note: mongoperf is an internal mongodb utility
// so we define the following macro
#define MONGO_EXPOSE_MACROS 1

#include "pch.h"

#include <iostream>

#include <boost/filesystem/operations.hpp>

#include "mongo/bson/util/atomic_int.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/util/logfile.h"
#include "mongo/util/mmap.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"


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

int syncDelaySecs = 0;

void syncThread() {
    while( 1 ) {
        mongo::Timer t;
        mmfFile->flush(true);
        cout << "                                                     mmf sync took " << t.millis() << "ms" << endl;
        sleepsecs(syncDelaySecs);
    }
}

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
    //cout << "read:" << r << " write:" << w << endl;
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
                lf->readAt(rofs, a.addr(), PG);
                iops++;
            }
            if( w ) {
                lf->writeAt(wofs, a.addr(), PG);
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
    verify( options["r"].trueValue() || options["w"].trueValue() );
    MemoryMappedFile f;
    cout << "creating test file size:";
    len = options["fileSizeMB"].numberLong();
    if( len == 0 ) len = 1;
    cout << len << "MB ..." << endl;

    if( 0 && len > 2000 && !options["mmf"].trueValue() ) { 
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
        verify( mmf );

        syncDelaySecs = options["syncDelay"].numberInt();
        if( syncDelaySecs ) {
            boost::thread t(syncThread);
        }
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
        if( i++ % 8 == 0 ) {
            if( nthr < wthr ) {
                while( nthr < wthr && nthr < d ) {
                    nthr++;
                    boost::thread w(workerThread);
                }
                cout << "new thread, total running : " << nthr << endl;
                d *= 2;
            }
        }
        sleepsecs(1);
        unsigned long long w = iops.get();
        iops.zero();
        w /= 1; // 1 secs
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
"    nThreads:<n>,     // number of threads (default 1)\n"
"    fileSizeMB:<n>,   // test file size (default 1MB)\n"
"    sleepMicros:<n>,  // pause for sleepMicros/nThreads between each operation (default 0)\n"
"    mmf:<bool>,       // if true do i/o's via memory mapped files (default false)\n"
"    r:<bool>,         // do reads (default false)\n"
"    w:<bool>,         // do writes (default false)\n"
"    syncDelay:<n>     // secs between fsyncs, like --syncdelay in mongod. (default 0/never)\n"
"  }\n"
"\n"
"mongoperf is a performance testing tool. the initial tests are of disk subsystem performance; \n"
"  tests of mongos and mongod will be added later.\n"
"most fields are optional.\n"
"non-mmf io is direct io (no caching). use a large file size to test making the heads\n"
"  move significantly and to avoid i/o coalescing\n"
"mmf io uses caching (the file system cache).\n"
"\n"

<< endl;
            return EXIT_SUCCESS;
        }

        cout << "use -h for help" << endl;

        char input[1024];
        memset(input, 0, sizeof(input));
        cin.read(input, 1000);
        if( *input == 0 ) { 
            cout << "error no options found on stdin for mongoperf" << endl;
            return EXIT_FAILURE;
        }

        string s = input;
        mongoutils::str::stripTrailing(s, " \n\r\0x1a");
        try { 
            options = fromjson(s);
        }
        catch(...) { 
            cout << "couldn't parse json options. input was:\n|" << s << "|" << endl;
            return EXIT_FAILURE;
        }
        cout << "parsed options:\n" << options.toString() << endl;

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
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

