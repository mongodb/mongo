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
 */

#include "mongo/pch.h"

#include <boost/filesystem/operations.hpp>

#include "mongo/db/storage/durable_mapped_file.h"
#include "mongo/util/timer.h"
#include "mongo/dbtests/dbtests.h"

namespace MMapTests {

    class LeakTest  {
        const string fn;
        const int optOld;
    public:
        LeakTest() :
            fn( (boost::filesystem::path(dbpath) / "testfile.map").string() ), optOld(cmdLine.durOptions)
        { 
            cmdLine.durOptions = 0; // DurParanoid doesn't make sense with this test
        }
        ~LeakTest() {
            cmdLine.durOptions = optOld;
            try { boost::filesystem::remove(fn); }
            catch(...) { }
        }
        void run() {

            try { boost::filesystem::remove(fn); }
            catch(...) { }

            Lock::GlobalWrite lk;

            {
                DurableMappedFile f;
                unsigned long long len = 256 * 1024 * 1024;
                verify( f.create(fn, len, /*sequential*/false) );
                {
                    char *p = (char *) f.getView();
                    verify(p);
                    // write something to the private view as a test
                    if( cmdLine.dur ) 
                        MemoryMappedFile::makeWritable(p, 6);
                    strcpy(p, "hello");
                }
                if( cmdLine.dur ) {
                    char *w = (char *) f.view_write();
                    strcpy(w + 6, "world");
                }
                MongoFileFinder ff;
                ASSERT( ff.findByPath(fn) );
                ASSERT( ff.findByPath("asdf") == 0 );
            }
            {
                MongoFileFinder ff;
                ASSERT( ff.findByPath(fn) == 0 );
            }

            int N = 10000;
#if !defined(_WIN32) && !defined(__linux__)
            // seems this test is slow on OS X.
            N = 100;
#endif

            // we make a lot here -- if we were leaking, presumably it would fail doing this many.
            Timer t;
            for( int i = 0; i < N; i++ ) {
                DurableMappedFile f;
                verify( f.open(fn, i%4==1) );
                {
                    char *p = (char *) f.getView();
                    verify(p);
                    if( cmdLine.dur ) 
                        MemoryMappedFile::makeWritable(p, 4);
                    strcpy(p, "zzz");
                }
                if( cmdLine.dur ) {
                    char *w = (char *) f.view_write();
                    if( i % 2 == 0 )
                        ++(*w);
                    verify( w[6] == 'w' );
                }
            }
            if( t.millis() > 10000 ) {
                mongo::unittest::log() << "warning: MMap LeakTest is unusually slow N:" << N <<
                    ' ' << t.millis() << "ms" << endl;
            }

        }
    };

    class All : public Suite {
    public:
        All() : Suite( "mmap" ) {}
        void setupTests() {
            add< LeakTest >();
        }
    } myall;

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
