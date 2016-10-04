/*    Copyright 2009 10gen Inc.
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
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

/*
   How to build and run:

   scons mongoperf
   ./mongoperf -h
*/


// note: mongoperf is an internal mongodb utility
// so we define the following macro
#define MONGO_EXPOSE_MACROS 1

#include "mongo/platform/basic.h"

#include <iostream>

#include <boost/filesystem/operations.hpp>

#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/storage/mmap_v1/logfile.h"
#include "mongo/db/storage/mmap_v1/mmap.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/allocator.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"

using namespace std;
using namespace mongo;
using namespace mongoutils::str;

int dummy;
unsigned recSizeKB;
LogFile* lf = 0;
MemoryMappedFile* mmfFile;
char* mmf = 0;
bo options;
unsigned long long len;  // file len
const unsigned PG = 4096;
unsigned nThreadsRunning = 0;

// as this is incremented A LOT, at some point this becomes a bottleneck if very high ops/second (in
// cache) things are happening.
AtomicUInt32 iops;

SimpleMutex m;

int syncDelaySecs = 0;

void syncThread() {
    while (1) {
        mongo::Timer t;
        mmfFile->flush(true);
        cout << "                                                     mmf sync took " << t.millis()
             << "ms" << endl;
        sleepsecs(syncDelaySecs);
    }
}

void stripTrailing(std::string& s, const char* chars) {
    std::string::iterator to = s.begin();
    for (std::string::iterator i = s.begin(); i != s.end(); i++) {
        // During each iteration if i finds a non-"chars" character it writes it to the
        // position of t. So the part of the string left from the "to" iterator is already
        // "cleared" string.
        if (!contains(chars, *i)) {
            if (i != to)
                s.replace(to, to + 1, 1, *i);
            to++;
        }
    }
    s.erase(to, s.end());
}

char* round(char* x) {
    size_t f = (size_t)x;
    char* p = (char*)((f + PG - 1) / PG * PG);
    return p;
}

struct Aligned {
    char x[8192];
    char* addr() {
        return round(x);
    }
};

unsigned long long rrand() {
    // RAND_MAX is very small on windows
    return (static_cast<unsigned long long>(rand()) << 15) ^ rand();
}

void workerThread() {
    bool r = options["r"].trueValue();
    bool w = options["w"].trueValue();
    cout << "read:" << r << " write:" << w << endl;
    long long su = options["sleepMicros"].numberLong();
    Aligned a;
    while (1) {
        unsigned long long rofs = (rrand() * PG) % len;
        unsigned long long wofs = (rrand() * PG) % len;
        const unsigned P = PG / 1024;
        if (mmf) {
            if (r) {
                for (unsigned p = P; p <= recSizeKB; p += P) {
                    if (rofs < len)
                        dummy += mmf[rofs];
                    rofs += PG;
                }
                iops.fetchAndAdd(1);
            }
            if (w) {
                for (unsigned p = P; p <= recSizeKB; p += P) {
                    if (wofs < len)
                        mmf[wofs] = 3;
                    wofs += PG;
                }
                iops.fetchAndAdd(1);
            }
        } else {
            if (r) {
                lf->readAt(rofs, a.addr(), recSizeKB * 1024);
                iops.fetchAndAdd(1);
            }
            if (w) {
                lf->writeAt(wofs, a.addr(), recSizeKB * 1024);
                iops.fetchAndAdd(1);
            }
        }
        long long micros = su / nThreadsRunning;
        if (micros) {
            sleepmicros(micros);
        }
    }
}

void go() {
    verify(options["r"].trueValue() || options["w"].trueValue());

    recSizeKB = options["recSizeKB"].numberInt();
    if (recSizeKB == 0)
        recSizeKB = 4;
    verify(recSizeKB <= 64000 && recSizeKB > 0);

    MemoryMappedFile f;
    cout << "creating test file size:";
    len = options["fileSizeMB"].numberLong();
    if (len == 0)
        len = 1;
    cout << len << "MB ..." << endl;

    if (0 && len > 2000 && !options["mmf"].trueValue()) {
        // todo make tests use 64 bit offsets in their i/o -- i.e. adjust LogFile::writeAt and such
        cout << "\nsizes > 2GB not yet supported with mmf:false" << endl;
        return;
    }
    len *= 1024 * 1024;
    const char* fname = "./mongoperf__testfile__tmp";
    try {
        boost::filesystem::remove(fname);
    } catch (...) {
        cout << "error deleting file " << fname << endl;
        return;
    }
    lf = new LogFile(fname, true);
    // needs to be big as we are using synchronousAppend.  if we used a regular MongoFile it
    // wouldn't have to be
    const unsigned sz = 1024 * 1024 * 32;
    char* buf = (char*)mongoMalloc(sz + 4096);
    const char* p = round(buf);
    for (unsigned long long i = 0; i < len; i += sz) {
        lf->synchronousAppend(p, sz);
        if (i % (1024ULL * 1024 * 1024) == 0 && i) {
            cout << i / (1024ULL * 1024 * 1024) << "GB..." << endl;
        }
    }
    BSONObj& o = options;

    if (o["mmf"].trueValue()) {
        delete lf;
        lf = 0;
        mmfFile = new MemoryMappedFile();
        mmf = (char*)mmfFile->map(fname);
        verify(mmf);

        syncDelaySecs = options["syncDelay"].numberInt();
        if (syncDelaySecs) {
            stdx::thread t(syncThread);
            t.detach();
        }
    }

    cout << "testing..." << endl;

    cout << "options:" << o.toString() << endl;
    unsigned wthr = 1;
    if (!o["nThreads"].eoo()) {
        wthr = (unsigned)o["nThreads"].Int();
    }
    cout << "wthr " << wthr << endl;

    if (wthr < 1) {
        cout << "bad threads field value" << endl;
        return;
    }
    unsigned i = 0;
    unsigned d = 1;
    unsigned& nthr = nThreadsRunning;
    while (1) {
        if (i++ % 8 == 0) {
            if (nthr < wthr) {
                while (nthr < wthr && nthr < d) {
                    nthr++;
                    stdx::thread w(workerThread);
                    w.detach();
                }
                cout << "new thread, total running : " << nthr << endl;
                d *= 2;
            }
        }
        sleepsecs(1);
        unsigned long long w = iops.loadRelaxed();
        iops.store(0);
        w /= 1;  // 1 secs
        cout << w << " ops/sec ";
        if (mmf == 0)
            // only writing 4 bytes with mmf so we don't say this
            cout << (w * PG / 1024 / 1024) << " MB/sec";
        cout << endl;
    }
}

int main(int argc, char* argv[]) {
    try {
        cout << "mongoperf" << endl;

        if (argc > 1) {
            cout <<

                "\n"
                "usage:\n"
                "\n"
                "  mongoperf < myjsonconfigfile\n"
                "\n"
                "  {\n"
                "    nThreads:<n>,     // number of threads (default 1)\n"
                "    fileSizeMB:<n>,   // test file size (default 1MB)\n"
                "    sleepMicros:<n>,  // pause for sleepMicros/nThreads between each operation "
                "(default 0)\n"
                "    mmf:<bool>,       // if true do i/o's via memory mapped files (default "
                "false)\n"
                "    r:<bool>,         // do reads (default false)\n"
                "    w:<bool>,         // do writes (default false)\n"
                "    recSizeKB:<n>,    // size of each write (default 4KB)\n"
                "    syncDelay:<n>     // secs between fsyncs, like --syncdelay in mongod. "
                "(default 0/never)\n"
                "  }\n"
                "\n"
                "mongoperf is a performance testing tool. the initial tests are of disk subsystem "
                "performance; \n"
                "  tests of mongos and mongod will be added later.\n"
                "most fields are optional.\n"
                "non-mmf io is direct io (no caching). use a large file size to test making the "
                "heads\n"
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
        if (*input == 0) {
            cout << "error no options found on stdin for mongoperf" << endl;
            return EXIT_FAILURE;
        }

        string s = input;
        stripTrailing(s, " \n\r\0x1a");
        try {
            options = fromjson(s);
        } catch (...) {
            cout << "couldn't parse json options. input was:\n|" << s << "|" << endl;
            return EXIT_FAILURE;
        }
        cout << "parsed options:\n" << options.toString() << endl;
        ProcessInfo::initializeSystemInfo();

        go();
    } catch (DBException& e) {
        cout << "caught DBException " << e.toString() << endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
