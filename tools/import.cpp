// import.cpp

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

#include <boost/program_options.hpp>

#include <fcntl.h>

namespace mongo {

    namespace po = boost::program_options;

    namespace import {

        void drillDown( DBClientConnection & conn , path root ) {

            if ( is_directory( root ) ) {
                directory_iterator end;
                directory_iterator i(root);
                while ( i != end ) {
                    path p = *i;
                    drillDown( conn , p );
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

            out() << "\t going into namespace [" << ns << "]" << endl;

            MemoryMappedFile mmf;
            assert( mmf.map( root.string().c_str() ) );

            char * data = (char*)mmf.viewOfs();
            int read = 0;

            int num = 0;

            while ( read < mmf.length() ) {
                if ( ! *data ) {
                    out() << "\t ** got unexpected end of file **  continuing..." << endl;
                    break;
                }

                BSONObj o( data );

                conn.insert( ns.c_str() , o );

                read += o.objsize();
                data += o.objsize();

                if ( ! ( ++num % 1000 ) )
                    out() << "read " << read << "/" << mmf.length() << " bytes so far. " << num << " objects" << endl;
            }

            out() << "\t "  << num << " objects" << endl;

        }


        void go( const char * dbHost , const char * dirRoot ) {
            DBClientConnection conn;
            string errmsg;
            if ( ! conn.connect( dbHost , errmsg ) ) {
                out() << "couldn't connect : " << errmsg << endl;
                throw -11;
            }

            drillDown( conn , dirRoot );
        }
    } // namespace import

} // namespace mongo

using namespace mongo;

int main( int argc , char ** argv ) {

    boost::filesystem::path::default_name_check( boost::filesystem::no_check );

    po::options_description options("import parameters");
    options.add_options()
    ("help", "produce help message")
    ("host,h", po::value<string>() , "mongo host to connect to")
    ("dir" , po::value<string>(), "directory to import from" )
    ;

    po::positional_options_description argsOptions;
    argsOptions.add( "dir" , 1 );

    po::variables_map vm;

    po::store( po::command_line_parser( argc , argv ).
               options(options).positional(argsOptions).run(), vm );

    po::notify(vm);

    if ( vm.count("help") ) {
        options.print( cerr );
        return 1;
    }

    const char * host = "127.0.0.1";
    const char * dir = "dump";

    if ( vm.count( "host" ) )
        host = vm["host"].as<string>().c_str();

    if ( vm.count( "dir" ) )
        dir = vm["dir"].as<string>().c_str();

    out() << "mongo dump" << endl;
    out() << "\t host        \t" << host << endl;
    out() << "\t dir         \t" << dir << endl;

    import::go( host , dir );
    return 0;
}
