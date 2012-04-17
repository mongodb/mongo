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

#include "mongo/tools/tool.h"

#include <fstream>
#include <iostream>

#include "pcrecpp.h"

#include "mongo/db/namespace_details.h"
#include "mongo/util/file_allocator.h"
#include "mongo/util/password.h"
#include "mongo/util/version.h"
#include "mongo/client/dbclient_rs.h"
#include "mongo/db/json.h"

#include <boost/filesystem/operations.hpp>

using namespace std;
using namespace mongo;

namespace po = boost::program_options;

namespace mongo {

    CmdLine cmdLine;

    Tool::Tool( string name , DBAccess access , string defaultDB ,
                string defaultCollection , bool usesstdout ) :
        _name( name ) , _db( defaultDB ) , _coll( defaultCollection ) ,
        _usesstdout(usesstdout), _noconnection(false), _autoreconnect(false), _conn(0), _slaveConn(0), _paired(false) {

        _options = new po::options_description( "options" );
        _options->add_options()
        ("help","produce help message")
        ("verbose,v", "be more verbose (include multiple times for more verbosity e.g. -vvvvv)")
        ("version", "print the program's version and exit" )
        ;

        if ( access & REMOTE_SERVER )
            _options->add_options()
            ("host,h",po::value<string>(), "mongo host to connect to ( <set name>/s1,s2 for sets)" )
            ("port",po::value<string>(), "server port. Can also use --host hostname:port" )
            ("ipv6", "enable IPv6 support (disabled by default)")
#ifdef MONGO_SSL
            ("ssl", "use all for connections")
#endif

            ("username,u",po::value<string>(), "username" )
            ("password,p", new PasswordValue( &_password ), "password" )
            ;

        if ( access & LOCAL_SERVER )
            _options->add_options()
            ("dbpath",po::value<string>(), "directly access mongod database "
             "files in the given path, instead of connecting to a mongod  "
             "server - needs to lock the data directory, so cannot be "
             "used if a mongod is currently accessing the same path" )
            ("directoryperdb", "if dbpath specified, each db is in a separate directory" )
            ("journal", "enable journaling" )
            ;

        if ( access & SPECIFY_DBCOL )
            _options->add_options()
            ("db,d",po::value<string>(), "database to use" )
            ("collection,c",po::value<string>(), "collection to use (some commands)" )
            ;

        _hidden_options = new po::options_description( name + " hidden options" );

        /* support for -vv -vvvv etc. */
        for (string s = "vv"; s.length() <= 10; s.append("v")) {
            _hidden_options->add_options()(s.c_str(), "verbose");
        }
    }

    Tool::~Tool() {
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

    void Tool::printVersion(ostream &out) {
        out << _name << " version " << mongo::versionString;
        if (mongo::versionString[strlen(mongo::versionString)-1] == '-')
            out << " (commit " << mongo::gitVersion() << ")";
        out << endl;
    }
    int Tool::main( int argc , char ** argv ) {
        static StaticObserver staticObserver;

        cmdLine.prealloc = false;

        // The default value may vary depending on compile options, but for tools
        // we want durability to be disabled.
        cmdLine.dur = false;

#if( BOOST_VERSION >= 104500 )
    boost::filesystem::path::default_name_check( boost::filesystem2::no_check );
#else
    boost::filesystem::path::default_name_check( boost::filesystem::no_check );
#endif

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
        }
        catch (po::error &e) {
            cerr << "ERROR: " << e.what() << endl << endl;
            printHelp(cerr);
            return EXIT_BADOPTIONS;
        }

        // hide password from ps output
        for (int i=0; i < (argc-1); ++i) {
            if (!strcmp(argv[i], "-p") || !strcmp(argv[i], "--password")) {
                char* arg = argv[i+1];
                while (*arg) {
                    *arg++ = 'x';
                }
            }
        }

        if ( _params.count( "help" ) ) {
            printHelp(cout);
            return 0;
        }

        if ( _params.count( "version" ) ) {
            printVersion(cout);
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


#ifdef MONGO_SSL
        if (_params.count("ssl")) {
            mongo::cmdLine.sslOnNormalPorts = true;
        }
#endif

        preSetup();

        bool useDirectClient = hasParam( "dbpath" );

        if ( ! useDirectClient ) {
            _host = "127.0.0.1";
            if ( _params.count( "host" ) )
                _host = _params["host"].as<string>();

            if ( _params.count( "port" ) )
                _host += ':' + _params["port"].as<string>();

            if ( _noconnection ) {
                // do nothing
            }
            else {
                string errmsg;

                ConnectionString cs = ConnectionString::parse( _host , errmsg );
                if ( ! cs.isValid() ) {
                    cerr << "invalid hostname [" << _host << "] " << errmsg << endl;
                    return -1;
                }

                _conn = cs.connect( errmsg );
                if ( ! _conn ) {
                    cerr << "couldn't connect to [" << _host << "] " << errmsg << endl;
                    return -1;
                }

                (_usesstdout ? cout : cerr ) << "connected to: " << _host << endl;
            }

        }
        else {
            if ( _params.count( "directoryperdb" ) ) {
                directoryperdb = true;
            }
            verify( lastError.get( true ) );

            if (_params.count("journal")){
                cmdLine.dur = true;
            }

            Client::initThread("tools");
            _conn = new DBDirectClient();
            _host = "DIRECT";
            static string myDbpath = getParam( "dbpath" );
            dbpath = myDbpath.c_str();
            try {
                acquirePathLock();
            }
            catch ( DBException& ) {
                cerr << endl << "If you are running a mongod on the same "
                     "path you should connect to that instead of direct data "
                     "file access" << endl << endl;
                dbexit( EXIT_CLEAN );
                return -1;
            }

            FileAllocator::get()->start();

            dur::startup();
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
        catch ( DBException& e ) {
            cerr << "assertion: " << e.toString() << endl;
            ret = -1;
        }
	catch(const boost::filesystem::filesystem_error &fse) {
	    /*
	      https://jira.mongodb.org/browse/SERVER-2904

	      Simple tools that don't access the database, such as
	      bsondump, aren't throwing DBExceptions, but are throwing
	      boost exceptions.

	      The currently available set of error codes don't seem to match
	      boost documentation.  boost::filesystem::not_found_error
	      (from http://www.boost.org/doc/libs/1_31_0/libs/filesystem/doc/exception.htm)
	      doesn't seem to exist in our headers.  Also, fse.code() isn't
	      boost::system::errc::no_such_file_or_directory when this
	      happens, as you would expect.  And, determined from
	      experimentation that the command-line argument gets turned into
	      "\\?" instead of "/?" !!!
	     */
#if defined(_WIN32)
	    if (/*(fse.code() == boost::system::errc::no_such_file_or_directory) &&*/
		(fse.path1() == "\\?"))
		printHelp(cerr);
	    else
#endif // _WIN32
		cerr << "error: " << fse.what() << endl;

	    ret = -1;
	}

        if ( currentClient.get() )
            currentClient.get()->shutdown();

        if ( useDirectClient )
            dbexit( EXIT_CLEAN );
        return ret;
    }

    DBClientBase& Tool::conn( bool slaveIfPaired ) {
        if ( slaveIfPaired && _conn->type() == ConnectionString::SET ) {
            if (!_slaveConn)
                _slaveConn = &((DBClientReplicaSet*)_conn)->slaveConn();
            return *_slaveConn;
        }
        return *_conn;
    }

    bool Tool::isMaster() {
        if ( hasParam("dbpath") ) {
            return true;
        }

        BSONObj info;
        bool isMaster;
        bool ok = conn().isMaster(isMaster, &info);

        if (ok && !isMaster) {
            cerr << "ERROR: trying to write to non-master " << conn().toString() << endl;
            cerr << "isMaster info: " << info << endl;
            return false;
        }

        return true;
    }

    bool Tool::isMongos() {
        // TODO: when mongos supports QueryOption_Exaust add a version check (SERVER-2628)
        BSONObj isdbgrid;
        conn("true").simpleCommand("admin", &isdbgrid, "isdbgrid");
        return isdbgrid["isdbgrid"].trueValue();
    }

    void Tool::addFieldOptions() {
        add_options()
        ("fields,f" , po::value<string>() , "comma separated list of field names e.g. -f name,age" )
        ("fieldFile" , po::value<string>() , "file with fields names - 1 per line" )
        ;
    }

    void Tool::needFields() {

        if ( hasParam( "fields" ) ) {
            BSONObjBuilder b;

            string fields_arg = getParam("fields");
            pcrecpp::StringPiece input(fields_arg);

            string f;
            pcrecpp::RE re("([#\\w\\.\\s\\-]+),?" );
            while ( re.Consume( &input, &f ) ) {
                _fields.push_back( f );
                b.append( f , 1 );
            }

            _fieldsObj = b.obj();
            return;
        }

        if ( hasParam( "fieldFile" ) ) {
            string fn = getParam( "fieldFile" );
            if ( ! boost::filesystem::exists( fn ) )
                throw UserException( 9999 , ((string)"file: " + fn ) + " doesn't exist" );

            const int BUF_SIZE = 1024;
            char line[ 1024 + 128];
            ifstream file( fn.c_str() );

            BSONObjBuilder b;
            while ( file.rdstate() == ios_base::goodbit ) {
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

    /** 
     * Validate authentication on the server for the given dbname.  populates
     * level (if supplied) with the user's credentials.
     */
    void Tool::auth( string dbname, Auth::Level * level ) {

        if ( ! dbname.size() )
            dbname = _db;

        if ( ! ( _username.size() || _password.size() ) ) {
            // Make sure that we don't need authentication to connect to this db
            // findOne throws an AssertionException if it's not authenticated.
            if (_coll.size() > 0) {
                // BSONTools don't have a collection
                conn().findOne(getNS(), Query("{}"), 0, QueryOption_SlaveOk);
            }

            // set write-level access if authentication is disabled
            if ( level != NULL )
                *level = Auth::WRITE;

            return;
        }

        string errmsg;
        if ( _conn->auth( dbname , _username , _password , errmsg, true, level ) ) {
            return;
        }

        // try against the admin db
        if ( _conn->auth( "admin" , _username , _password , errmsg, true, level ) ) {
            return;
        }

        throw UserException( 9997 , (string)"authentication failed: " + errmsg );
    }

    BSONTool::BSONTool( const char * name, DBAccess access , bool objcheck )
        : Tool( name , access , "" , "" , false ) , _objcheck( objcheck ) {

        add_options()
        ("objcheck" , "validate object before inserting" )
        ("filter" , po::value<string>() , "filter to apply before inserting" )
        ;
    }


    int BSONTool::run() {
        _objcheck = hasParam( "objcheck" );

        if ( hasParam( "filter" ) )
            _matcher.reset( new Matcher( fromjson( getParam( "filter" ) ) ) );

        return doRun();
    }

    long long BSONTool::processFile( const boost::filesystem::path& root ) {
        _fileName = root.string();

        unsigned long long fileLength = file_size( root );

        if ( fileLength == 0 ) {
            out() << "file " << _fileName << " empty, skipping" << endl;
            return 0;
        }


        FILE* file = fopen( _fileName.c_str() , "rb" );
        if ( ! file ) {
            log() << "error opening file: " << _fileName << " " << errnoWithDescription() << endl;
            return 0;
        }

#if !defined(__sunos__) && defined(POSIX_FADV_SEQUENTIAL)
        posix_fadvise(fileno(file), 0, fileLength, POSIX_FADV_SEQUENTIAL);
#endif

        log(1) << "\t file size: " << fileLength << endl;

        unsigned long long read = 0;
        unsigned long long num = 0;
        unsigned long long processed = 0;

        const int BUF_SIZE = BSONObjMaxUserSize + ( 1024 * 1024 );
        boost::scoped_array<char> buf_holder(new char[BUF_SIZE]);
        char * buf = buf_holder.get();

        ProgressMeter m( fileLength );
        m.setUnits( "bytes" );

        while ( read < fileLength ) {
            size_t amt = fread(buf, 1, 4, file);
            verify( amt == 4 );

            int size = little<int>::ref( buf );
            uassert( 10264 , str::stream() << "invalid object size: " << size , size < BUF_SIZE );

            amt = fread(buf+4, 1, size-4, file);
            verify( amt == (size_t)( size - 4 ) );

            BSONObj o( buf );
            if ( _objcheck && ! o.valid() ) {
                cerr << "INVALID OBJECT - going try and pring out " << endl;
                cerr << "size: " << size << endl;
                BSONObjIterator i(o);
                while ( i.more() ) {
                    BSONElement e = i.next();
                    try {
                        e.validate();
                    }
                    catch ( ... ) {
                        cerr << "\t\t NEXT ONE IS INVALID" << endl;
                    }
                    cerr << "\t name : " << e.fieldName() << " " << e.type() << endl;
                    cerr << "\t " << e << endl;
                }
            }

            if ( _matcher.get() == 0 || _matcher->matches( o ) ) {
                gotObject( o );
                processed++;
            }

            read += o.objsize();
            num++;

            m.hit( o.objsize() );
        }

        fclose( file );

        uassert( 10265 ,  "counts don't match" , m.done() == fileLength );
        (_usesstdout ? cout : cerr ) << m.hits() << " objects found" << endl;
        if ( _matcher.get() )
            (_usesstdout ? cout : cerr ) << processed << " objects processed" << endl;
        return processed;
    }



    void setupSignals( bool inFork ) {}
}
