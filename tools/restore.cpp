// restore.cpp

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

#include "../stdafx.h"
#include "../client/dbclient.h"
#include "../util/mmap.h"
#include "Tool.h"

#include <boost/program_options.hpp>

#include <fcntl.h>

using namespace mongo;

namespace po = boost::program_options;

class Restore : public Tool {
public:
    Restore() : Tool( "restore" ){
        add_options()
            ("dir",po::value<string>() , "directory to restore from" )
            ;
        addPositionArg( "dir" , 1 );
    }
    
    int run(){
        drillDown( getParam( "dir" ) );
        return 0;
    }
    
    void drillDown( path root ) {

        if ( is_directory( root ) ) {
            directory_iterator end;
            directory_iterator i(root);
            while ( i != end ) {
                path p = *i;
                drillDown( p );
                i++;
            }
            return;
        }
        
        if ( ! ( endsWith( root.string().c_str() , ".bson" ) ||
                 endsWith( root.string().c_str() , ".bin" ) ) ) {
            cerr << "don't know what to do with [" << root.string() << "]" << endl;
            return;
        }
        
        out() << root.string() << endl;
        
        string ns;
        {
            string dir = root.branch_path().string();
            if ( dir.find( "/" ) == string::npos )
                ns += dir;
            else
                ns += dir.substr( dir.find_last_of( "/" ) + 1 );
        }
        
        {
            string l = root.leaf();
            l = l.substr( 0 , l.find_last_of( "." ) );
            ns += "." + l;
        }
        
        if ( boost::filesystem::file_size( root ) == 0 ) {
            out() << "file " + root.native_file_string() + " empty, aborting" << endl;
            return;
        }

        out() << "\t going into namespace [" << ns << "]" << endl;
        
        MemoryMappedFile mmf;
        long fileLength;
        assert( mmf.map( root.string().c_str() , fileLength ) );
        
        char * data = (char*)mmf.viewOfs();
        long read = 0;
        
        long num = 0;
        
        int msgDelay = 1000 * ( mmf.length() / ( 1024 * 1024 * 400 ) );
        while ( read < mmf.length() ) {
            BSONObj o( data );
            
            conn().insert( ns.c_str() , o );
            
            read += o.objsize();
            data += o.objsize();

            if ( ! ( ++num % msgDelay ) )
                out() << "read " << read << "/" << mmf.length() << " bytes so far. (" << (int)(read * 100 / mmf.length()) << "%) " << num << " objects" << endl;
        }
        
        out() << "\t "  << num << " objects" << endl;
    }
};

int main( int argc , char ** argv ) {
    Restore restore;
    return restore.main( argc , argv );
}
