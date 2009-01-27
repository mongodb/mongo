// Tool.cpp

#include "Tool.h"

#include <iostream>

#include <boost/filesystem/operations.hpp>

using namespace std;
using namespace mongo;

namespace po = boost::program_options;

mongo::Tool::Tool( string name , string defaultDB , string defaultCollection ) : 
    _name( name ) , _db( defaultDB ) , _coll( defaultCollection ){
    
    _options = new po::options_description( name + " options" );
    _options->add_options()
        ("help","produce help message")
        ("host,h",po::value<string>(), "mongo host to connect to" )
        ("db,d",po::value<string>(), "database to use" )
        ("collection,c",po::value<string>(), "collection to use (some commands)" )
        ;

}

mongo::Tool::~Tool(){
    delete( _options );
}

int mongo::Tool::main( int argc , char ** argv ){
    boost::filesystem::path::default_name_check( boost::filesystem::no_check );
    
    po::store( po::parse_command_line( argc, argv, *_options ), _params );
    po::notify( _params );

    if ( _params.count( "help" ) ){
        _options->print( cerr );
        return 0;
    }
    
    const char * host = "127.0.0.1";
    if ( _params.count( "host" ) )
        host = _params["host"].as<string>().c_str();
    
    string errmsg;
    if ( ! _conn.connect( host , errmsg ) ){
        cerr << "couldn't connect to [" << host << "] " << errmsg << endl;
        return -1;
    }
    
    cout << "connected to: " << host << endl;

    if ( _params.count( "db" ) )
        _db = _params["db"].as<string>();
    
    if ( _params.count( "collection" ) )
        _coll = _params["collection"].as<string>();

    run();

    return 0;
}
