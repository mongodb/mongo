/*
 *    Copyright (C) 2010 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/pch.h"

#include <fcntl.h>
#include <stdio.h>

#include "mongo/util/goodies.h"

namespace mongo {

#if defined(F_FULLFSYNC)
    void fullsync(int f) {
        fcntl( f, F_FULLFSYNC );
    }
#else
    void fullsync(int f) {
        fdatasync(f);
    }
#endif

    int main(int argc, char* argv[], char *envp[] ) {
        cout << "hello" << endl;

        FILE *f = fopen("/data/db/temptest", "a");

        if ( f == 0 ) {
            cout << "can't open file\n";
            return 1;
        }

        {
            Timer t;
            for ( int i = 0; i < 50000; i++ )
                fwrite("abc", 3, 1, f);
            cout << "small writes: " << t.millis() << "ms" << endl;
        }

        {
            Timer t;
            for ( int i = 0; i < 10000; i++ ) {
                fwrite("abc", 3, 1, f);
                fflush(f);
                fsync( fileno( f ) );
            }
            int ms = t.millis();
            cout << "flush: " << ms << "ms, " << ms / 10000.0 << "ms/request" << endl;
        }

        {
            Timer t;
            for ( int i = 0; i < 500; i++ ) {
                fwrite("abc", 3, 1, f);
                fflush(f);
                fsync( fileno( f ) );
                sleepmillis(2);
            }
            int ms = t.millis() - 500 * 2;
            cout << "flush with sleeps: " << ms << "ms, " << ms / 500.0 << "ms/request" << endl;
        }

        char buf[8192];
        for ( int pass = 0; pass < 2; pass++ ) {
            cout << "pass " << pass << endl;
            {
                Timer t;
                int n = 500;
                for ( int i = 0; i < n; i++ ) {
                    if ( pass == 0 )
                        fwrite("abc", 3, 1, f);
                    else
                        fwrite(buf, 8192, 1, f);
                    buf[0]++;
                    fflush(f);
                    fullsync(fileno(f));
                }
                int ms = t.millis();
                cout << "fullsync: " << ms << "ms, " << ms / ((double) n) << "ms/request" << endl;
            }

            {
                Timer t;
                for ( int i = 0; i < 500; i++ ) {
                    if ( pass == 0 )
                        fwrite("abc", 3, 1, f);
                    else
                        fwrite(buf, 8192, 1, f);
                    buf[0]++;
                    fflush(f);
                    fullsync(fileno(f));
                    sleepmillis(2);
                }
                int ms = t.millis() - 2 * 500;
                cout << "fullsync with sleeps: " << ms << "ms, " << ms / 500.0 << "ms/request" << endl;
            }
        }

        // without growing
        {
            fclose(f);
            /* try from beginning of the file, where we aren't appending and changing the file length,
               to see if this is faster as the directory entry then doesn't have to be flushed (if noatime in effect).
            */
            f = fopen("/data/db/temptest", "r+");
            Timer t;
            int n = 500;
            for ( int i = 0; i < n; i++ ) {
                fwrite("xyz", 3, 1, f);
                fflush(f);
                fullsync(fileno(f));
            }
            int ms = t.millis();
            cout << "fullsync without growing: " << ms << "ms, " << ms / ((double) n) << "ms/request" << endl;
        }

        // without growing, with delay
        {
            fclose(f);
            /* try from beginning of the file, where we aren't appending and changing the file length,
               to see if this is faster as the directory entry then doesn't have to be flushed (if noatime in effect).
            */
            f = fopen("/data/db/temptest", "r+");
            Timer t;
            int n = 500;
            for ( int i = 0; i < n; i++ ) {
                fwrite("xyz", 3, 1, f);
                fflush(f);
                fullsync(fileno(f));
                sleepmillis(2);
            }
            int ms = t.millis() - 2 * 500;
            cout << "fullsync without growing with sleeps: " << ms << "ms, " << ms / ((double) n) << "ms/request" << endl;
        }

        return 0;
    }

} // namespace mongo
