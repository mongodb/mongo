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

#include "tool.h"

#include <fstream>
#include <iostream>

#include <boost/program_options.hpp>

using namespace mongo;

namespace po = boost::program_options;

class ImportJSON : public Tool {
public:
    ImportJSON() : Tool( "importjson" ){
        add_options()
            ("file",po::value<string>() , "file to import from; if not specified stdin is used" )
            ("idbefore", "create id index before importing " )
            ("id", "create id index after importing (recommended) " )
            ("drop", "drop collection first " )
            ;
        addPositionArg( "file" , 1 );
    }

    int run(){
        string filename = getParam( "file" );
        long long fileSize = -1;

        istream * in = &cin;

        ifstream file( filename.c_str() , ios_base::in | ios_base::binary);

        if ( filename.size() > 0 && filename != "-" ){
            in = &file;
            fileSize = file_size( filename );
        }

        string ns;

        try {
            ns = getNS();
        } catch (...) {
            printHelp(cerr);
            return -1;
        }
        
        auth();

        if ( hasParam( "drop" ) ){
            cout << "dropping: " << ns << endl;
            conn().dropCollection( ns.c_str() );
        }

        if ( hasParam( "idbefore" ) ){
            conn().ensureIndex( ns.c_str() , BSON( "_id" << 1 ) );
        }

        int num = 0;

        time_t start = time(0);

        ProgressMeter pm( fileSize );
        const int BUF_SIZE = 1024 * 1024 * 4;
        char line[ (1024 * 1024 * 4) + 128];
        while ( *in ){
            in->getline( line , BUF_SIZE );
            
            char * buf = line;
            while( isspace( buf[0] ) ) buf++;

            int len = strlen( buf );
            if ( ! len )
                continue;
            
            if ( in->rdstate() == ios_base::eofbit )
                break;
            assert( in->rdstate() == 0 );

            try {
                BSONObj o = fromjson( buf );
                conn().insert( ns.c_str() , o );
            }
            catch ( MsgAssertionException& ma ){
                cout << "exception:" << ma.toString() << endl;
                cout << buf << endl;
            }

            num++;
            if ( pm.hit( len + 1 ) ){
                cout << "\t\t\t" << num << "\t" << ( num / ( time(0) - start ) ) << "/second" << endl;
            }
        }

        if ( hasParam( "id" ) ){
            conn().ensureIndex( ns.c_str() , BSON( "_id" << 1 ) );
        }

        return 0;
    }
};

int main( int argc , char ** argv ) {
    ImportJSON import;
    return import.main( argc , argv );
}
