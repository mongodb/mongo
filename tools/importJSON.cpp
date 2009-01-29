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

#ifdef MODERN_BOOST
#include <boost/iostreams/copy.hpp>
#include <boost/iostreams/filtering_streambuf.hpp>
#include <boost/iostreams/filter/gzip.hpp>

using namespace boost::iostreams;
#endif

using namespace mongo;


namespace po = boost::program_options;


class ImportJSON : public Tool {
public:
    ImportJSON() : Tool( "importjson" ){
        add_options()
            ("file",po::value<string>() , "file to import from" )
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

        ifstream file( filename.c_str() , ios_base::in | ios_base::binary);
        
#ifdef MODERN_BOOST
        
        filtering_streambuf<input> in;
        in.push(gzip_decompressor());
        in.push(file);
        boost::iostreams::copy(in, cout);
#else
        istream & in = file;
#endif
        
        string ns = getNS();

        if ( hasParam( "drop" ) ){
            cout << "dropping: " << ns << endl;
            _conn.dropCollection( ns.c_str() );
        }
        
        while ( in ){
            string line;
            getline( in , line );
            
            if ( line.size() == 0 )
                break;
            
            try {
                BSONObj o = fromjson( line );
                _conn.insert( ns.c_str() , o );
            }
            catch ( MsgAssertionException ma ){
                cout << "exception:" << ma.toString() << endl;
                cout << line << endl;
            }
        }
        
        return 0;
    }
};

int main( int argc , char ** argv ) {
    ImportJSON import;
    return import.main( argc , argv );
}
