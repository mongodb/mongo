// Tool.cpp

#include "Tool.h"

#include <iostream>

#include <boost/filesystem/operations.hpp>

#include "util/file_allocator.h"

using namespace std;
using namespace mongo;

namespace po = boost::program_options;

mongo::Tool::Tool( string name , string defaultDB , string defaultCollection ) :
    _name( name ) , _db( defaultDB ) , _coll( defaultCollection ) , _conn(0), _paired(false) {

    _options = new po::options_description( name + " options" );
    _options->add_options()
        ("help","produce help message")
        ("host,h",po::value<string>(), "mongo host to connect to" )
        ("db,d",po::value<string>(), "database to use" )
        ("collection,c",po::value<string>(), "collection to use (some commands)" )
        ("username,u",po::value<string>(), "username" )
        ("password,p",po::value<string>(), "password" )
        ("dbpath",po::value<string>(), "directly access mongod data files in this path, instead of connecting to a mongod instance" )
        ("verbose,v", "be more verbose (include multiple times for more verbosity e.g. -vvvvv)")
        ;

}

mongo::Tool::~Tool(){
    delete( _options );
    if ( _conn )
        delete _conn;
}

void mongo::Tool::printExtraHelp( ostream & out ){
}

void mongo::Tool::printHelp(ostream &out) {
    _options->print(out);
    printExtraHelp(out);
}

int mongo::Tool::main( int argc , char ** argv ){
    boost::filesystem::path::default_name_check( boost::filesystem::no_check );

    po::store( po::command_line_parser( argc , argv ).
               options( *_options ).
               positional( _positonalOptions ).run() , _params );

    po::notify( _params );

    if ( _params.count( "help" ) ){
        printHelp(cerr);
        return 0;
    }

    if ( _params.count( "verbose" ) )
        logLevel = 1;

    if ( ! hasParam( "dbpath" ) ) {
        _host = "127.0.0.1";
        if ( _params.count( "host" ) )
            _host = _params["host"].as<string>();
        
        if ( _host.find( "," ) == string::npos ){
            DBClientConnection * c = new DBClientConnection();
            _conn = c;
            
            string errmsg;
            if ( ! c->connect( _host , errmsg ) ){
                cerr << "couldn't connect to [" << _host << "] " << errmsg << endl;
                return -1;
            }
        }
        else {
            DBClientPaired * c = new DBClientPaired();
            _paired = true;
            _conn = c;
            
            if ( ! c->connect( _host ) ){
                cerr << "couldn't connect to paired server: " << _host << endl;
                return -1;
            }
        }

        cerr << "connected to: " << _host << endl;
    } 
    else {
        _conn = new DBDirectClient();
        _host = "DIRECT";
        static string myDbpath = getParam( "dbpath" );
        mongo::dbpath = myDbpath.c_str();
        mongo::acquirePathLock();
        theFileAllocator().start();
    }

    if ( _params.count( "db" ) )
        _db = _params["db"].as<string>();

    if ( _params.count( "collection" ) )
        _coll = _params["collection"].as<string>();
    
    if ( _params.count( "username" ) )
        _username = _params["username"].as<string>();

    if ( _params.count( "password" ) )
        _password = _params["password"].as<string>();
    
    try {
        return run();
    }
    catch ( DBException& e ){
        cerr << "assertion: " << e.toString() << endl;
        return -1;
    }
}

mongo::DBClientBase& mongo::Tool::conn( bool slaveIfPaired ){
    if ( _paired && slaveIfPaired )
        return ((DBClientPaired*)_conn)->slaveConn();
    return *_conn;
}

void mongo::Tool::auth( string dbname ){
    if ( ! dbname.size() )
        dbname = _db;

    if ( ! ( _username.size() || _password.size() ) )
        return;

    string errmsg;
    if ( _conn->auth( dbname , _username , _password , errmsg ) )
        return;
    
    // try against the admin db
    string err2;
    if ( _conn->auth( "admin" , _username , _password , errmsg ) )
        return;
    
    throw mongo::UserException( (string)"auth failed: " + errmsg );
}
