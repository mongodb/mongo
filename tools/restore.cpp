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
    Restore() : Tool( "restore" , "" ){
        add_options()
            ("dir", po::value<string>()->default_value("dump"), "directory to restore from")
            ;
    }

    int run(){
        auth();
        path root = getParam("dir");
        if (!is_directory(root)) {
            cerr << "\"" << root.string() << "\" is not a valid directory" << endl;
            return EXIT_BADOPTIONS;
        }

        /* If _db is not "" then the user specified a db name to restore as.
         *
         * In that case we better be given a root directory that contains only
         * .bson files (a db)
         */
        drillDown(root, _db != "");
        return EXIT_CLEAN;
    }

    void drillDown( path root, bool use_db = false ) {
        log(2) << "drillDown: " << root.string() << endl;

        if ( is_directory( root ) ) {
            directory_iterator end;
            directory_iterator i(root);
            while ( i != end ) {
                path p = *i;

                if (use_db) {
                    if (is_directory(p) ||
                        !(endsWith(p.string().c_str(), ".bson") ||
                          endsWith(p.string().c_str(), ".bin" ))) {
                        cerr << "ERROR: root directory must be a dump of a single database" << endl;
                        cerr << "       when specifying a db name with --db" << endl;
                        printHelp(cout);
                        return;
                    }
                }

                drillDown(p, use_db);
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
        if (use_db) {
            ns += _db;
        } else {
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

        long long fileLength = file_size( root );

        if ( fileLength == 0 ) {
            out() << "file " + root.native_file_string() + " empty, skipping" << endl;
            return;
        }

        out() << "\t going into namespace [" << ns << "]" << endl;

        string fileString = root.string();
        ifstream file( fileString.c_str() , ios_base::in | ios_base::binary);
        if ( ! file.is_open() ){
            log() << "error opening file: " << fileString << endl;
            return;
        }

        log(1) << "\t file size: " << fileLength << endl;

        long long read = 0;
        long long num = 0;

        int msgDelay = (int)(1000 * ( 1 + ( fileLength / ( 1024.0 * 1024 * 400 ) ) ) );
        log(1) << "\t msg delay: " << msgDelay << endl;

        const int BUF_SIZE = 1024 * 1024 * 5;
        char * buf = (char*)malloc( BUF_SIZE );

        while ( read < fileLength ) {
            file.read( buf , 4 );
            int size = ((int*)buf)[0];
            assert( size < BUF_SIZE );

            file.read( buf + 4 , size - 4 );

            BSONObj o( buf );
            conn().insert( ns.c_str() , o );

            read += o.objsize();
            num++;

            if ( ( logLevel > 0 && num < 10 ) || ! ( num % msgDelay ) )
                out() << "read " << read << "/" << fileLength << " bytes so far. (" << (int)( (read * 100) / fileLength) << "%) " << num << " objects" << endl;
        }

        free( buf );
        out() << "\t "  << num << " objects" << endl;
    }
};

int main( int argc , char ** argv ) {
    Restore restore;
    return restore.main( argc , argv );
}
