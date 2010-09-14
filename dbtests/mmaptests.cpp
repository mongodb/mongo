// socktests.cpp : sock.{h,cpp} unit tests.
//

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

#include "pch.h"
#include "../util/mmap.h"
#include "dbtests.h"

namespace MMapTests {

#if 0

    class CopyOnWriteSpeedTest {
    public:
        void run() {

            string fn = "/tmp/testfile.map";
            boost::filesystem::remove(fn);

            MemoryMappedFile f;
            char *p = (char *) f.create(fn, 1024 * 1024 * 1024, true);
            assert(p);
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
                    assert( q );
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
                    assert( q );
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
                    assert( q );
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
                    assert( q );
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
        All() : Suite( "mmap" ){}
        void setupTests(){
            add< CopyOnWriteSpeedTest >();
        }
    } myall;

#endif

}
