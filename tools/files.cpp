// files.cpp

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
#include "client/gridfs.h"
#include "client/dbclient.h"

#include "Tool.h"

#include <fstream>
#include <iostream>

#include <boost/program_options.hpp>

using namespace mongo;

namespace po = boost::program_options;

class Files : public Tool {
public:
    Files() : Tool( "files" ){
        add_options()
            ( "command" , po::value<string>() , "command (list|put|get)" )
            ( "file" , po::value<string>() , "filename for get|put" )
            ;
        addPositionArg( "command" , 1 );
        addPositionArg( "file" , 2 );
    }
    
    int run(){
        string cmd = getParam( "command" );
        if ( cmd.size() == 0 ){
            cerr << "need command" << endl;
            return -1;
        }
        
        GridFS g( _conn , _db );

        if ( cmd == "list" ){
            auto_ptr<DBClientCursor> c = g.list();
            while ( c->more() ){
                BSONObj obj = c->next();
                cout 
                    << obj["filename"].str() << "\t" 
                    << (long)obj["length"].number() 
                    << endl;
            }
            return 0;
        }
        
        string filename = getParam( "file" );
        if ( filename.size() == 0 ){
            cerr << "need a filename" << endl;
            return -1;
        }

        if ( cmd == "get" ){
            GridFile f = g.findFile( filename );
            if ( ! f.exists() ){
                cerr << "file not found" << endl;
                return -2;
            }

            string out = f.getFilename();
            f.write( out );
            cout << "done write to: " << out << endl;
            return 0;
        }
        
        if ( cmd == "put" ){
            g.storeFile( filename );
            cout << "done!";
            return 0;
        }
        
        cerr << "unknown command: " << cmd << endl;
        return -1;
    }
};

int main( int argc , char ** argv ) {
    Files f;
    return f.main( argc , argv );
}
