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

#include "../pch.h"
#include "../client/dbclient.h"
#include "../util/mmap.h"
#include "tool.h"

#include <boost/program_options.hpp>

#include <fcntl.h>

using namespace mongo;

namespace po = boost::program_options;

class Restore : public BSONTool {
public:
    
    bool _drop;
    const char * _curns;

    Restore() : BSONTool( "restore" ) , _drop(false){
        add_options()
            ("drop" , "drop each collection before import" )
            ;
        add_hidden_options()
            ("dir", po::value<string>()->default_value("dump"), "directory to restore from")
            ("indexesLast" , "wait to add indexes (now default)") // left in for backwards compatibility
            ;
        addPositionArg("dir", 1);
    }

    virtual void printExtraHelp(ostream& out) {
        out << "usage: " << _name << " [options] [directory or filename to restore from]" << endl;
    }

    virtual int doRun(){
        auth();
        path root = getParam("dir");

        // check if we're actually talking to a machine that can write
        if (!isMaster()) {
            return -1;
        }
        
        _drop = hasParam( "drop" );

        /* If _db is not "" then the user specified a db name to restore as.
         *
         * In that case we better be given either a root directory that
         * contains only .bson files or a single .bson file  (a db).
         *
         * In the case where a collection name is specified we better be
         * given either a root directory that contains only a single
         * .bson file, or a single .bson file itself (a collection).
         */
        drillDown(root, _db != "", _coll != "");
        conn().getLastError();
        return EXIT_CLEAN;
    }

    void drillDown( path root, bool use_db = false, bool use_coll = false ) {
        log(2) << "drillDown: " << root.string() << endl;

        if ( is_directory( root ) ) {
            directory_iterator end;
            directory_iterator i(root);
            path indexes;
            while ( i != end ) {
                path p = *i;
                i++;

                if (use_db) {
                    if (is_directory(p)) {
                        cerr << "ERROR: root directory must be a dump of a single database" << endl;
                        cerr << "       when specifying a db name with --db" << endl;
                        printHelp(cout);
                        return;
                    }
                }

                if (use_coll) {
                    if (is_directory(p) || i != end) {
                        cerr << "ERROR: root directory must be a dump of a single collection" << endl;
                        cerr << "       when specifying a collection name with --collection" << endl;
                        printHelp(cout);
                        return;
                    }
                }

                if ( p.leaf() == "system.indexes.bson" )
                    indexes = p;
                else
                    drillDown(p, use_db, use_coll);
            }

            if (!indexes.empty())
                drillDown(indexes, use_db, use_coll);

            return;
        }

        if ( ! ( endsWith( root.string().c_str() , ".bson" ) ||
                 endsWith( root.string().c_str() , ".bin" ) ) ) {
            cerr << "don't know what to do with [" << root.string() << "]" << endl;
            return;
        }

        log() << root.string() << endl;

        if ( root.leaf() == "system.profile.bson" ){
            log() << "\t skipping" << endl;
            return;
        }

        string ns;
        if (use_db) {
            ns += _db;
        } 
        else {
            string dir = root.branch_path().string();
            if ( dir.find( "/" ) == string::npos )
                ns += dir;
            else
                ns += dir.substr( dir.find_last_of( "/" ) + 1 );
            
            if ( ns.size() == 0 )
                ns = "test";
        }
        
        assert( ns.size() );

        if (use_coll) {
            ns += "." + _coll;
        } else {
            string l = root.leaf();
            l = l.substr( 0 , l.find_last_of( "." ) );
            ns += "." + l;
        }

        out() << "\t going into namespace [" << ns << "]" << endl;

        if ( _drop ){
            out() << "\t dropping" << endl;
            conn().dropCollection( ns );
        }
        
        _curns = ns.c_str();
        processFile( root );
    }

    virtual void gotObject( const BSONObj& obj ){
        conn().insert( _curns , obj );
    }

    
};

int main( int argc , char ** argv ) {
    Restore restore;
    return restore.main( argc , argv );
}
