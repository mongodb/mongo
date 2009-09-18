// dbtests.cpp : Runs db unit tests.
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

#include "stdafx.h"

#include "../db/instance.h"
#include "../util/file_allocator.h"

#if !defined(_WIN32)
#include <sys/file.h>
#endif

#include "dbtests.h"

using namespace std;

namespace mongo {
    extern string dbpath;
}

string dbpathSpec = "/tmp/unittest/";

void usage() {
    string instructions =
        "dbtests usage:\n"
        "  -help           show this message\n"
        "  -dbpath <path>  configure db data path for this test run\n"
        "                  (default is /tmp/unittest/)\n"
        "  -debug          run tests with verbose output\n"
        "  -list           list available test suites\n"
        "  -seed <seed>    random number seed\n"
        "  <suite>         run the specified test suite only";
    out() << instructions << endl;
}

int main( int argc, char** argv ) {

    unsigned long long seed = time( 0 );
    
    int offset = 0;
    for ( int i = 1; i < argc; ++i ) {
        if ( argv[ i ] == string( "-dbpath" ) ) {
            if ( i == argc - 1 ) {
                usage();
                dbexit( EXIT_BADOPTIONS );
            }
            dbpathSpec = argv[ ++i ];
            offset += 2;
        } else if ( argv[ i ] == string( "-seed" ) ) {
            if ( i == argc - 1 ) {
                usage();
                dbexit( EXIT_BADOPTIONS );
            }
            // Don't bother checking for conversion error
            seed = strtoll( argv[ ++i ], 0, 10 );
            offset += 2;
        } else if ( argv[ i ] == string( "-help" ) ) {
            usage();
            dbexit( EXIT_CLEAN );
        } else if ( offset ) {
            argv[ i - offset ] = argv[ i ];
        }
    }
    argc -= offset;

    if ( dbpathSpec[ dbpathSpec.length() - 1 ] != '/' )
        dbpathSpec += "/";
    dbpath = dbpathSpec.c_str();

    acquirePathLock();
    
    srand( seed );
    printGitVersion();
    printSysInfo();
    out() << "random seed: " << seed << endl;

    theFileAllocator().start();

    

    int ret = mongo::regression::Suite::run( argc, argv );
    
#if !defined(_WIN32) && !defined(__sunos__)
    flock( lockFile, LOCK_UN );
#endif    

    dbexit( (ExitCode)ret ); // so everything shuts down cleanly
    return ret;
}
