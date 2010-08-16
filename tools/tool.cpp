/*
 *    Copyright (C) 2010 10gen Inc.
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

// Tool.cpp

#include "tool.h"

#include <iostream>

#include <boost/filesystem/operations.hpp>
#include <pcrecpp.h>

#include "util/file_allocator.h"
#include "util/password.h"

using namespace std;
using namespace mongo;

namespace po = boost::program_options;

namespace mongo {

    CmdLine cmdLine;

    Tool::Tool( string name , DBAccess access , string defaultDB , 
                string defaultCollection , bool usesstdout ) :
        _name( name ) , _db( defaultDB ) , _coll( defaultCollection ) , 
        _usesstdout(usesstdout), _noconnection(false), _autoreconnect(false), _conn(0), _paired(false) {
        
        _options = new po::options_description( "options" );
        _options->add_options()
            ("help","produce help message")
            ("verbose,v", "be more verbose (include multiple times for more verbosity e.g. -vvvvv)")
            ;

        if ( access == ALL || access == NO_LOCAL){
            _options->add_options()
                ("host,h",po::value<string>(), "mongo host to connect to (\"left,right\" for pairs)" )
                ("port",po::value<string>(), "server port. Can also use --host hostname:port" )
                ("db,d",po::value<string>(), "database to use" )
                ("collection,c",po::value<string>(), "collection to use (some commands)" )
                ("username,u",po::value<string>(), "username" )
                ("password,p", new PasswordValue( &_password ), "password" )
                ("ipv6", "enable IPv6 support (disabled by default)")
                ;
        }

        if ( access == ALL ) {
            _options->add_options()
                ("dbpath",po::value<string>(), "directly access mongod database "
                 "files in the given path, instead of connecting to a mongod  "
                 "server - needs to lock the data directory, so cannot be "
                 "used if a mongod is currently accessing the same path" )
                ("directoryperdb", "if dbpath specified, each db is in a separate directory" )
                ;
        }

        _hidden_options = new po::options_description( name + " hidden options" );

        /* support for -vv -vvvv etc. */
        for (string s = "vv"; s.length() <= 10; s.append("v")) {
            _hidden_options->add_options()(s.c_str(), "verbose");
        }
    }

    Tool::~Tool(){
        delete( _options );
        delete( _hidden_options );
        if ( _conn )
            delete _conn;
    }

    void Tool::printHelp(ostream &out) {
        printExtraHelp(out);
        _options->print(out);
        printExtraHelpAfter(out);
    }

    int Tool::main( int argc , char ** argv ){
        static StaticObserver staticObserver;
        
        cmdLine.prealloc = false;

        boost::filesystem::path::default_name_check( boost::filesystem::no_check );

        _name = argv[0];

        /* using the same style as db.cpp */
        int command_line_style = (((po::command_line_style::unix_style ^
                                    po::command_line_style::allow_guessing) |
                                   po::command_line_style::allow_long_disguise) ^
                                  po::command_line_style::allow_sticky);
        try {
            po::options_description all_options("all options");
            all_options.add(*_options).add(*_hidden_options);

            po::store( po::command_line_parser( argc , argv ).
                       options(all_options).
                       positional( _positonalOptions ).
                       style(command_line_style).run() , _params );

            po::notify( _params );
        } catch (po::error &e) {
            cerr << "ERROR: " << e.what() << endl << endl;
            printHelp(cerr);
            return EXIT_BADOPTIONS;
        }

        // hide password from ps output
        for (int i=0; i < (argc-1); ++i){
            if (!strcmp(argv[i], "-p") || !strcmp(argv[i], "--password")){
                char* arg = argv[i+1];
                while (*arg){
                    *arg++ = 'x';
                }
            }
        }

        if ( _params.count( "help" ) ){
            printHelp(cout);
            return 0;
        }

        if ( _params.count( "verbose" ) ) {
            logLevel = 1;
        }

        for (string s = "vv"; s.length() <= 10; s.append("v")) {
            if (_params.count(s)) {
                logLevel = s.length();
            }
        }
        
        preSetup();

        bool useDirectClient = hasParam( "dbpath" );
        
        if ( ! useDirectClient ) {
            _host = "127.0.0.1";
            if ( _params.count( "host" ) )
                _host = _params["host"].as<string>();

            if ( _params.count( "port" ) )
                _host += ':' + _params["port"].as<string>();
            
            if ( _noconnection ){
                // do nothing
            }
            else {
                string errmsg;

                ConnectionString cs = ConnectionString::parse( _host , errmsg );
                if ( ! cs.isValid() ){
                    cerr << "invalid hostname [" << _host << "] " << errmsg << endl;
                    return -1;
                }
                
                _conn = cs.connect( errmsg );
                if ( ! _conn ){
                    cerr << "couldn't connect to [" << _host << "] " << errmsg << endl;
                    return -1;
                }
            }
            
            (_usesstdout ? cout : cerr ) << "connected to: " << _host << endl;
        }
        else {
            if ( _params.count( "directoryperdb" ) ) {
                directoryperdb = true;
            }
            assert( lastError.get( true ) );
            Client::initThread("tools");
            _conn = new DBDirectClient();
            _host = "DIRECT";
            static string myDbpath = getParam( "dbpath" );
            dbpath = myDbpath.c_str();
            try {
                acquirePathLock();
            }
            catch ( DBException& ){
                cerr << endl << "If you are running a mongod on the same "
                    "path you should connect to that instead of direct data "
                    "file access" << endl << endl;
                dbexit( EXIT_CLEAN );
                return -1;
            }

            theFileAllocator().start();
        }

        if ( _params.count( "db" ) )
            _db = _params["db"].as<string>();

        if ( _params.count( "collection" ) )
            _coll = _params["collection"].as<string>();

        if ( _params.count( "username" ) )
            _username = _params["username"].as<string>();

        if ( _params.count( "password" )
             && ( _password.empty() ) ) {
            _password = askPassword();
        }

        if (_params.count("ipv6"))
            enableIPv6();

        int ret = -1;
        try {
            ret = run();
        }
        catch ( DBException& e ){
            cerr << "assertion: " << e.toString() << endl;
            ret = -1;
        }
    
        if ( currentClient.get() )
            currentClient->shutdown();

        if ( useDirectClient )
            dbexit( EXIT_CLEAN );
        return ret;
    }

    DBClientBase& Tool::conn( bool slaveIfPaired ){
        // TODO: _paired is deprecated
        if ( slaveIfPaired && _conn->type() == ConnectionString::SET )
            return ((DBClientReplicaSet*)_conn)->slaveConn();
        return *_conn;
    }

    void Tool::addFieldOptions(){
        add_options()
            ("fields,f" , po::value<string>() , "comma separated list of field names e.g. -f name,age" )
            ("fieldFile" , po::value<string>() , "file with fields names - 1 per line" )
            ;
    }

    void Tool::needFields(){

        if ( hasParam( "fields" ) ){
            BSONObjBuilder b;
        
            string fields_arg = getParam("fields");
            pcrecpp::StringPiece input(fields_arg);
        
            string f;
            pcrecpp::RE re("([#\\w\\.\\s\\-]+),?" );
            while ( re.Consume( &input, &f ) ){
                _fields.push_back( f );
                b.append( f , 1 );
            }
        
            _fieldsObj = b.obj();
            return;
        }

        if ( hasParam( "fieldFile" ) ){
            string fn = getParam( "fieldFile" );
            if ( ! exists( fn ) )
                throw UserException( 9999 , ((string)"file: " + fn ) + " doesn't exist" );

            const int BUF_SIZE = 1024;
            char line[ 1024 + 128];
            ifstream file( fn.c_str() );

            BSONObjBuilder b;
            while ( file.rdstate() == ios_base::goodbit ){
                file.getline( line , BUF_SIZE );
                const char * cur = line;
                while ( isspace( cur[0] ) ) cur++;
                if ( cur[0] == '\0' )
                    continue;

                _fields.push_back( cur );
                b.append( cur , 1 );
            }
            _fieldsObj = b.obj();
            return;
        }

        throw UserException( 9998 , "you need to specify fields" );
    }

    void Tool::auth( string dbname ){
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

        throw UserException( 9997 , (string)"auth failed: " + errmsg );
    }

    BSONTool::BSONTool( const char * name, DBAccess access , bool objcheck ) 
        : Tool( name , access , "" , "" ) , _objcheck( objcheck ){
        
        add_options()
            ("objcheck" , "validate object before inserting" )
            ("filter" , po::value<string>() , "filter to apply before inserting" )
            ;
    }


    int BSONTool::run(){
        _objcheck = hasParam( "objcheck" );
        
        if ( hasParam( "filter" ) )
            _matcher.reset( new Matcher( fromjson( getParam( "filter" ) ) ) );
        
        return doRun();
    }

    long long BSONTool::processFile( const path& root ){
        string fileString = root.string();
        
        long long fileLength = file_size( root );

        if ( fileLength == 0 ) {
            out() << "file " << fileString << " empty, skipping" << endl;
            return 0;
        }


        FILE* file = fopen( fileString.c_str() , "rb" );
        if ( ! file ){
            log() << "error opening file: " << fileString << endl;
            return 0;
        }

#if !defined(__sunos__) && defined(POSIX_FADV_SEQUENTIAL)
        posix_fadvise(fileno(file), 0, fileLength, POSIX_FADV_SEQUENTIAL);
#endif

        log(1) << "\t file size: " << fileLength << endl;

        long long read = 0;
        long long num = 0;
        long long processed = 0;

        const int BUF_SIZE = 1024 * 1024 * 5;
        boost::scoped_array<char> buf_holder(new char[BUF_SIZE]);
        char * buf = buf_holder.get();

        ProgressMeter m( fileLength );

        while ( read < fileLength ) {
            int readlen = fread(buf, 4, 1, file);
            int size = ((int*)buf)[0];
            if ( size >= BUF_SIZE ){
                cerr << "got an object of size: " << size << "  terminating..." << endl;
            }
            uassert( 10264 ,  "invalid object size" , size < BUF_SIZE );

            readlen = fread(buf+4, size-4, 1, file);

            BSONObj o( buf );
            if ( _objcheck && ! o.valid() ){
                cerr << "INVALID OBJECT - going try and pring out " << endl;
                cerr << "size: " << size << endl;
                BSONObjIterator i(o);
                while ( i.more() ){
                    BSONElement e = i.next();
                    try {
                        e.validate();
                    }
                    catch ( ... ){
                        cerr << "\t\t NEXT ONE IS INVALID" << endl;
                    }
                    cerr << "\t name : " << e.fieldName() << " " << e.type() << endl;
                    cerr << "\t " << e << endl;
                }
            }
            
            if ( _matcher.get() == 0 || _matcher->matches( o ) ){
                gotObject( o );
                processed++;
            }

            read += o.objsize();
            num++;

            m.hit( o.objsize() );
        }

        uassert( 10265 ,  "counts don't match" , m.done() == fileLength );
        out() << "\t "  << m.hits() << " objects found" << endl;
        if ( _matcher.get() )
            out() << "\t "  << processed << " objects processed" << endl;
        return processed;
    }
            


    void setupSignals(){}
}
