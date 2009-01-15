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

#include "dbtests.h"

#include <unittest/Registry.hpp>

using namespace std;

namespace mongo {
    extern const char* dbpath;
} // namespace mongo
string dbpathSpec = "/tmp/unittest/";

void usage() {
    string instructions =
        "dbtests usage:\n"
        "  -help           show this message\n"
        "  -dbpath <path>  configure db data path for this test run\n"
        "                  (default is /tmp/unittest/)\n"
        "  -debug          run tests with verbose output\n"
        "  -list           list available test suites\n"
        "  <suite>         run the specified test suite only";
    cout << instructions << endl;
}

int main( int argc, char** argv ) {

    int offset = 0;
    for ( int i = 1; i < argc; ++i ) {
        if ( argv[ i ] == string( "-dbpath" ) ) {
            if ( i == argc - 1 ) {
                usage();
                exit( -1 );
            }
            dbpathSpec = argv[ ++i ];
            offset += 2;
        } else if ( argv[ i ] == string( "-help" ) ) {
            usage();
            exit( 0 );
        } else if ( offset ) {
            argv[ i - offset ] = argv[ i ];
        }
    }
    argc -= offset;

    if ( dbpathSpec[ dbpathSpec.length() - 1 ] != '/' )
        dbpathSpec += "/";
    dbpath = dbpathSpec.c_str();

    UnitTest::Registry tests;

    tests.add( btreeTests(), "btree" );
    tests.add( jsobjTests(), "jsobj" );
    tests.add( namespaceTests(), "namespace" );
    tests.add( pairingTests(), "pairing" );
    tests.add( pdfileTests(), "pdfile" );

    return tests.run( argc, argv );
}
