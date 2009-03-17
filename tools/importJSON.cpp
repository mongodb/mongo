// importJSON.cpp

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
#include "client/dbclient.h"
#include "db/json.h"

#include "Tool.h"

#include <fstream>
#include <iostream>

#include <boost/program_options.hpp>

using namespace mongo;

namespace po = boost::program_options;

class ImportJSON : public Tool {
public:
    ImportJSON() : Tool( "importjson" ){
        add_options()
            ("file",po::value<string>() , "file to import from" )
            ("idbefore", "create id index before importing " )
            ("id", "create id index after importing (recommended) " )
            ("drop", "drop collection first " )
            ;
        addPositionArg( "file" , 1 );
    }

    int run(){
        string filename = getParam( "file" );
        if ( filename.size() == 0 ){
            cerr << "need to specify a file!" << endl;
            return -1;

        }

        istream * in = &cin;

        ifstream file( filename.c_str() , ios_base::in | ios_base::binary);

        if ( filename != "-" ){
            in = &file;
        }

        string ns = getNS();

        if ( hasParam( "drop" ) ){
            cout << "dropping: " << ns << endl;
            _conn.dropCollection( ns.c_str() );
        }

        if ( hasParam( "idbefore" ) ){
            _conn.ensureIndex( ns.c_str() , BSON( "_id" << 1 ) );
        }

        int num = 0;

        time_t start = time(0);

        const int BUF_SIZE = 64000;
        char line[64000 + 128];
        while ( *in ){
            in->getline( line , BUF_SIZE );

            int len = strlen( line );
            if ( ! len )
                break;

            assert( len < BUF_SIZE );

            try {
                BSONObj o = fromjson( line );
                _conn.insert( ns.c_str() , o );
            }
            catch ( MsgAssertionException& ma ){
                cout << "exception:" << ma.toString() << endl;
                cout << line << endl;
            }

            if ( ++num % 10000 == 0 ){
                cout << num << "\t" << ( num / ( time(0) - start ) ) << "/second" << endl;

            }
        }

        if ( hasParam( "id" ) ){
            _conn.ensureIndex( ns.c_str() , BSON( "_id" << 1 ) );
        }

        return 0;
    }
};

int main( int argc , char ** argv ) {
    ImportJSON import;
    return import.main( argc , argv );
}
