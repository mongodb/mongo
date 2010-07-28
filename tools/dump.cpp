// dump.cpp

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
#include "tool.h"

#include <fcntl.h>

using namespace mongo;

namespace po = boost::program_options;

class Dump : public Tool {
public:
    Dump() : Tool( "dump" , true , "*" ){
        add_options()
            ("out,o", po::value<string>()->default_value("dump"), "output directory")
            ("query,q", po::value<string>() , "json query" )
            ;
    }

    void doCollection( const string coll , path outputFile ) {
        cout << "\t" << coll << " to " << outputFile.string() << endl;
        
        ofstream out;
        out.open( outputFile.string().c_str() , ios_base::out | ios_base::binary  );
        assertStreamGood( 10262 ,  "couldn't open file" , out );

        ProgressMeter m( conn( true ).count( coll.c_str() , BSONObj() , QueryOption_SlaveOk ) );

        Query q;
        if ( _query.isEmpty() )
            q.snapshot();
        else
            q = _query;

        auto_ptr<DBClientCursor> cursor = conn( true ).query( coll.c_str() , q , 0 , 0 , 0 , QueryOption_SlaveOk | QueryOption_NoCursorTimeout );

        while ( cursor->more() ) {
            BSONObj obj = cursor->next();
            out.write( obj.objdata() , obj.objsize() );
            m.hit();
        }

        cout << "\t\t " << m.done() << " objects" << endl;

        out.close();
    }

    void go( const string db , const path outdir ) {
        cout << "DATABASE: " << db << "\t to \t" << outdir.string() << endl;

        create_directories( outdir );

        string sns = db + ".system.namespaces";
        
        auto_ptr<DBClientCursor> cursor = conn( true ).query( sns.c_str() , Query() , 0 , 0 , 0 , QueryOption_SlaveOk | QueryOption_NoCursorTimeout );
        while ( cursor->more() ) {
            BSONObj obj = cursor->next();
            if ( obj.toString().find( ".$" ) != string::npos )
                continue;

            const string name = obj.getField( "name" ).valuestr();
            const string filename = name.substr( db.size() + 1 );

            if ( _coll.length() > 0 && db + "." + _coll != name && _coll != name )
                continue;

            doCollection( name.c_str() , outdir / ( filename + ".bson" ) );

        }

    }
    
    int run(){
        
        {
            string q = getParam("query");
            if ( q.size() )
                _query = fromjson( q );
        }

        path root( getParam("out") );
        string db = _db;

        if ( db == "*" ){
            cout << "all dbs" << endl;
            auth( "admin" );

            BSONObj res = conn( true ).findOne( "admin.$cmd" , BSON( "listDatabases" << 1 ) );
            BSONObj dbs = res.getField( "databases" ).embeddedObjectUserCheck();
            set<string> keys;
            dbs.getFieldNames( keys );
            for ( set<string>::iterator i = keys.begin() ; i != keys.end() ; i++ ) {
                string key = *i;

                BSONObj dbobj = dbs.getField( key ).embeddedObjectUserCheck();

                const char * dbName = dbobj.getField( "name" ).valuestr();
                if ( (string)dbName == "local" )
                    continue;

                go ( dbName , root / dbName );
            }
        }
        else {
            auth( db );
            go( db , root / db );
        }
        return 0;
    }

    BSONObj _query;
};

int main( int argc , char ** argv ) {
    Dump d;
    return d.main( argc , argv );
}
