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

#include "../stdafx.h"
#include "../client/dbclient.h"

#include <boost/program_options.hpp>

#include <fcntl.h>

namespace po = boost::program_options;

namespace dump {
    
    void doCollection( DBClientConnection & conn , const char * coll , path outputFile ){
        cout << "\t" << coll << " to " << outputFile.string() << endl;
        
        int out = open( outputFile.string().c_str() , O_WRONLY | O_CREAT | O_TRUNC , 0666 );
        assert( out );
        
        BSONObjBuilder query;
        auto_ptr<DBClientCursor> cursor = conn.query( coll , query.doneAndDecouple() );
        
        int num = 0;
        while ( cursor->more() ){    
            BSONObj obj = cursor->next();
            write( out , obj.objdata() , obj.objsize() );
            num++;
        }
        
        cout << "\t\t " << num << " objects" << endl;

        close( out );
    }
    
    void go( DBClientConnection & conn , const char * db , const path outdir ){
        cout << "DATABASE: " << db << endl;
        
        create_directories( outdir );

        string sns = db;
        sns += ".system.namespaces";
        
        BSONObjBuilder query;
        auto_ptr<DBClientCursor> cursor = conn.query( sns.c_str() , query.doneAndDecouple() );
        while ( cursor->more() ){
            BSONObj obj = cursor->next();
            if ( obj.toString().find( ".$" ) != string::npos )
                continue;
            
            const string name = obj.getField( "name" ).valuestr();
            const string filename = name.substr( strlen( db ) + 1 );
            
            doCollection( conn , name.c_str() , outdir / ( filename + ".bson" ) );

        }
        
    }

    void go( const char * host , const char * db , const char * outdir ){
        DBClientConnection conn;
        string errmsg;
        if ( ! conn.connect( host , errmsg ) ){
            cout << "couldn't connect : " << errmsg << endl;
            throw -11;
        }
        
        path root(outdir);
        
        if ( strlen( db ) == 1 && db[0] == '*' ){            
            cout << "all dbs" << endl;

            BSONObjBuilder query;
            query.appendBool( "listDatabases" , 1 );
    
            BSONObj res = conn.findOne( "admin.$cmd" , query.doneAndDecouple() );
            BSONObj dbs = res.getField( "databases" ).embeddedObjectUserCheck();
            set<string> keys;
            dbs.getFieldNames( keys );
            for ( set<string>::iterator i = keys.begin() ; i != keys.end() ; i++ ){
                string key = *i;
                
                BSONObj db = dbs.getField( key ).embeddedObjectUserCheck();
                
                const char * dbName = db.getField( "name" ).valuestr();
                if ( (string)dbName == "local" )
                    continue;
                go ( conn , dbName , root / dbName );
            }
        }
        else {
            go( conn , db , root / db );
        }
    }

}


int main( int argc , char ** argv ){

 boost:filesystem::path::default_name_check( boost::filesystem::no_check );

    po::options_description options("dump parameters");
    options.add_options()
        ("help", "produce help message")
        ("host,h", po::value<string>() , "mongo host to connecto to")
        ("db,d" , po::value<string>() , "database to dump" )
        ("out" , po::value<string>() , "output directory" )
        ;
    
    po::variables_map vm;       
    po::store(po::parse_command_line(argc, argv, options), vm);
    po::notify(vm);   
    
    if ( vm.count("help") ){
        options.print( cerr );
        return 1;
    }

    const char * host = "127.0.0.1";
    const char * db = "*";
    const char * outdir = "dump";

    if ( vm.count( "host" ) )
        host = vm["host"].as<string>().c_str();
    
    if ( vm.count( "db" ) )
        db = vm["db"].as<string>().c_str();
    
    if ( vm.count( "out" ) )
        outdir = vm["db"].as<string>().c_str();
    
    cout << "mongo dump" << endl;
    cout << "\t host        \t" << host << endl;
    cout << "\t db          \t" << db << endl;
    cout << "\t output dir  \t" << outdir << endl;

    dump::go( host , db , outdir );
}
