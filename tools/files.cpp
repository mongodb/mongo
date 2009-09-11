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
        add_hidden_options()
            ( "command" , po::value<string>() , "command (list|search|put|get)" )
            ( "file" , po::value<string>() , "filename for get|put" )
            ;
        addPositionArg( "command" , 1 );
        addPositionArg( "file" , 2 );
    }

    virtual void printExtraHelp( ostream & out ){
        out << "usage: " << _name << " [options] command [filename]" << endl;
        out << "command:" << endl;
        out << "  one of (list|search|put|get)" << endl;
        out << "  list - list all files.  takes an optional prefix " << endl;
        out << "         which listed filenames must begin with." << endl;
        out << "  search - search all files. takes an optional substring " << endl;
        out << "           which listed filenames must contain." << endl;
        out << "  put - add a file" << endl;
        out << "  get - get a file" << endl;
    }

    void display( GridFS * grid , BSONObj obj ){
        auto_ptr<DBClientCursor> c = grid->list( obj );
        while ( c->more() ){
            BSONObj obj = c->next();
            cout
                << obj["filename"].str() << "\t"
                << (long)obj["length"].number()
                << endl;
        }
    }

    int run(){
        string cmd = getParam( "command" );
        if ( cmd.size() == 0 ){
            cerr << "need command" << endl;
            return -1;
        }

        GridFS g( conn() , _db );
        auth();

        string filename = getParam( "file" );

        if ( cmd == "list" ){
            BSONObjBuilder b;
            if ( filename.size() )
                b.appendRegex( "filename" , ( (string)"^" + filename ).c_str() );
            display( &g , b.obj() );
            return 0;
        }

        if ( filename.size() == 0 ){
            cerr << "need a filename" << endl;
            return -1;
        }

        if ( cmd == "search" ){
            BSONObjBuilder b;
            b.appendRegex( "filename" , filename.c_str() );
            display( &g , b.obj() );
            return 0;
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
            cout << "file object: " << g.storeFile( filename ) << endl;
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
