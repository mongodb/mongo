// Tool.cpp

#include "Tool.h"

#include <iostream>

#include <boost/filesystem/operations.hpp>

#include "util/file_allocator.h"

using namespace std;
using namespace mongo;

namespace po = boost::program_options;

mongo::Tool::Tool( string name , string defaultDB , string defaultCollection ) : 
    _name( name ) , _db( defaultDB ) , _coll( defaultCollection ), _useDirect() {
    
    _options = new po::options_description( name + " options" );
    _options->add_options()
        ("help","produce help message")
        ("host,h",po::value<string>(), "mongo host to connect to" )
        ("db,d",po::value<string>(), "database to use" )
        ("collection,c",po::value<string>(), "collection to use (some commands)" )
        ("dbpath",po::value<string>(), "directly access mongod data files in this path, instead of connecting to a mongod instance" )
        ;

}

mongo::Tool::~Tool(){
    delete( _options );
}

void mongo::Tool::printExtraHelp( ostream & out ){
}

int mongo::Tool::main( int argc , char ** argv ){
    boost::filesystem::path::default_name_check( boost::filesystem::no_check );
    
    po::store( po::command_line_parser( argc , argv ).
               options( *_options ).
               positional( _positonalOptions ).run() , _params );

    po::notify( _params );

    if ( _params.count( "help" ) ){
        _options->print( cerr );
        printExtraHelp( cerr );
        return 0;
    }

    if ( !hasParam( "dbpath" ) ) {
        const char * host = "127.0.0.1";
        if ( _params.count( "host" ) )
            host = _params["host"].as<string>().c_str();
        
        string errmsg;
        if ( ! _conn.connect( host , errmsg ) ){
            cerr << "couldn't connect to [" << host << "] " << errmsg << endl;
            return -1;
        }
        
        cerr << "connected to: " << host << endl;
    } else {
        _useDirect = true;
        static string myDbpath = getParam( "dbpath" );
        mongo::dbpath = myDbpath.c_str();
        mongo::acquirePathLock();
        theFileAllocator().start();
    }
    
    if ( _params.count( "db" ) )
        _db = _params["db"].as<string>();
    
    if ( _params.count( "collection" ) )
        _coll = _params["collection"].as<string>();

    return run();
}
