// @file mmaptests.cpp

/**
 *    Copyright (C) 2008 10gen Inc.
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

#include "mongo/platform/basic.h"

#include <boost/filesystem/operations.hpp>
#include <iostream>

#include "mongo/db/concurrency/lock_state.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/mmap_v1/data_file.h"
#include "mongo/db/storage/mmap_v1/durable_mapped_file.h"
#include "mongo/db/storage/mmap_v1/extent.h"
#include "mongo/db/storage/mmap_v1/extent_manager.h"
#include "mongo/db/storage/mmap_v1/mmap_v1_extent_manager.h"
#include "mongo/db/storage/mmap_v1/mmap_v1_options.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/util/timer.h"

namespace MMapTests {

using std::endl;
using std::string;

class LeakTest {
    const string fn;
    const int optOld;

public:
    LeakTest()
        : fn((boost::filesystem::path(storageGlobalParams.dbpath) / "testfile.map").string()),
          optOld(mmapv1GlobalOptions.journalOptions) {
        mmapv1GlobalOptions.journalOptions = 0;  // DurParanoid doesn't make sense with this test
    }
    ~LeakTest() {
        mmapv1GlobalOptions.journalOptions = optOld;
        try {
            boost::filesystem::remove(fn);
        } catch (...) {
        }
    }
    void run() {
        try {
            boost::filesystem::remove(fn);
        } catch (...) {
        }

        MMAPV1LockerImpl lockState;
        Lock::GlobalWrite lk(&lockState);

        {
            DurableMappedFile f;
            unsigned long long len = 256 * 1024 * 1024;
            verify(f.create(fn, len));
            {
                char* p = (char*)f.getView();
                verify(p);
                // write something to the private view as a test
                if (storageGlobalParams.dur)
                    privateViews.makeWritable(p, 6);
                strcpy(p, "hello");
            }
            if (storageGlobalParams.dur) {
                char* w = (char*)f.view_write();
                strcpy(w + 6, "world");
            }
            MongoFileFinder ff;
            ASSERT(ff.findByPath(fn));
            ASSERT(ff.findByPath("asdf") == 0);
        }
        {
            MongoFileFinder ff;
            ASSERT(ff.findByPath(fn) == 0);
        }

        int N = 10000;
#if !defined(_WIN32) && !defined(__linux__)
        // seems this test is slow on OS X.
        N = 100;
#endif

        // we make a lot here -- if we were leaking, presumably it would fail doing this many.
        Timer t;
        for (int i = 0; i < N; i++) {
            // Every 4 iterations we pass the sequential hint.
            DurableMappedFile f{i % 4 == 1 ? MongoFile::Options::SEQUENTIAL
                                           : MongoFile::Options::NONE};
            verify(f.open(fn));
            {
                char* p = (char*)f.getView();
                verify(p);
                if (storageGlobalParams.dur)
                    privateViews.makeWritable(p, 4);
                strcpy(p, "zzz");
            }
            if (storageGlobalParams.dur) {
                char* w = (char*)f.view_write();
                if (i % 2 == 0)
                    ++(*w);
                verify(w[6] == 'w');
            }
        }
        if (t.millis() > 10000) {
            mongo::unittest::log() << "warning: MMap LeakTest is unusually slow N:" << N << ' '
                                   << t.millis() << "ms" << endl;
        }
    }
};

class ExtentSizing {
public:
    void run() {
        MmapV1ExtentManager em("x", "x", false);

        ASSERT_EQUALS(em.maxSize(), em.quantizeExtentSize(em.maxSize()));

        // test that no matter what we start with, we always get to max extent size
        for (int obj = 16; obj < BSONObjMaxUserSize; obj += 111) {
            int sz = em.initialSize(obj);

            double totalExtentSize = sz;

            int numFiles = 1;
            int sizeLeftInExtent = em.maxSize() - 1;

            for (int i = 0; i < 100; i++) {
                sz = em.followupSize(obj, sz);
                ASSERT(sz >= obj);
                ASSERT(sz >= em.minSize());
                ASSERT(sz <= em.maxSize());
                ASSERT(sz <= em.maxSize());

                totalExtentSize += sz;

                if (sz < sizeLeftInExtent) {
                    sizeLeftInExtent -= sz;
                } else {
                    numFiles++;
                    sizeLeftInExtent = em.maxSize() - sz;
                }
            }
            ASSERT_EQUALS(em.maxSize(), sz);

            double allocatedOnDisk = (double)numFiles * em.maxSize();

            ASSERT((totalExtentSize / allocatedOnDisk) > .95);

            invariant(em.numFiles() == 0);
        }
    }
};

class All : public Suite {
public:
    All() : Suite("mmap") {}
    void setupTests() {
        if (!getGlobalServiceContext()->getGlobalStorageEngine()->isMmapV1())
            return;

        add<LeakTest>();
        add<ExtentSizing>();
    }
};

SuiteInstance<All> myall;

#if 0

    class CopyOnWriteSpeedTest {
    public:
        void run() {

            string fn = "/tmp/testfile.map";
            boost::filesystem::remove(fn);

            MemoryMappedFile f;
            char *p = (char *) f.create(fn, 1024 * 1024 * 1024, true);
            verify(p);
            strcpy(p, "hello");

            {
                void *x = f.testGetCopyOnWriteView();
                Timer tt;
                for( int i = 11; i < 1000000000; i++ )
                    p[i] = 'z';
                cout << "fill 1GB time: " << tt.millis() << "ms" << endl;
                f.testCloseCopyOnWriteView(x);
            }

            /* test a lot of view/unviews */
            {
                Timer t;

                char *q;
                for( int i = 0; i < 1000; i++ ) {
                    q = (char *) f.testGetCopyOnWriteView();
                    verify( q );
                    if( i == 999 ) {
                        strcpy(q+2, "there");
                    }
                    f.testCloseCopyOnWriteView(q);
                }

                cout << "view unview: " << t.millis() << "ms" << endl;
            }

            f.flush(true);

            /* plain old mmaped writes */
            {
                Timer t;
                for( int i = 0; i < 10; i++ ) {
                    memset(p+100, 'c', 200 * 1024 * 1024);
                }
                cout << "traditional writes: " << t.millis() << "ms" << endl;
            }

            f.flush(true);

            /* test doing some writes */
            {
                Timer t;
                char *q = (char *) f.testGetCopyOnWriteView();
                for( int i = 0; i < 10; i++ ) {
                    verify( q );
                    memset(q+100, 'c', 200 * 1024 * 1024);
                }
                f.testCloseCopyOnWriteView(q);

                cout << "inc style some writes: " << t.millis() << "ms" << endl;
            }

            /* test doing some writes */
            {
                Timer t;
                for( int i = 0; i < 10; i++ ) {
                    char *q = (char *) f.testGetCopyOnWriteView();
                    verify( q );
                    memset(q+100, 'c', 200 * 1024 * 1024);
                    f.testCloseCopyOnWriteView(q);
                }

                cout << "some writes: " << t.millis() << "ms" << endl;
            }

            /* more granular */
            {
                Timer t;
                for( int i = 0; i < 100; i++ ) {
                    char *q = (char *) f.testGetCopyOnWriteView();
                    verify( q );
                    memset(q+100, 'c', 20 * 1024 * 1024);
                    f.testCloseCopyOnWriteView(q);
                }

                cout << "more granular some writes: " << t.millis() << "ms" << endl;
            }

            p[10] = 0;
            cout << p << endl;
        }
    };

    class All : public Suite {
    public:
        All() : Suite( "mmap" ) {}
        void setupTests() {
            add< CopyOnWriteSpeedTest >();
        }
    } myall;

#endif
}
