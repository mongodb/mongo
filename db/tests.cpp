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

/* tests.cpp

   unit test & such
*/

#include "pch.h"
#include "../util/mmap.h"

namespace mongo {

    int test2_old9() {
        out() << "test2" << endl;
        printStackTrace();
        if ( 1 )
            return 1;

        MemoryMappedFile f;

        unsigned long long len = 64*1024*1024;
        char *p = (char *) f.map("/tmp/test.dat", len);
        char *start = p;
        char *end = p + 64*1024*1024-2;
        end[1] = 'z';
        int i;
        while ( p < end ) {
            *p++ = ' ';
            if ( ++i%64 == 0 ) {
                *p++ = '\n';
                *p++ = 'x';
            }
        }
        *p = 'a';

        f.flush(true);
        out() << "done" << endl;

        char *x = start + 32 * 1024 * 1024;
        char *y = start + 48 * 1024 * 1024;
        char *z = start + 62 * 1024 * 1024;

        strcpy(z, "zfoo");
        out() << "y" << endl;
        strcpy(y, "yfoo");
        strcpy(x, "xfoo");
        strcpy(start, "xfoo");

        dbexit( EXIT_TEST );

        return 1;
    }

} // namespace mongo
