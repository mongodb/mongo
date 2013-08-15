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

#include <boost/filesystem/operations.hpp>
#include <fstream>
#include <iostream>

#include "pcrecpp.h"

#include "mongo/base/initializer.h"
#include "mongo/client/dbclient_rs.h"
#include "mongo/client/sasl_client_authenticate.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/authz_manager_external_state_mock.h"
#include "mongo/db/json.h"
#include "mongo/db/namespace_details.h"
#include "mongo/platform/posix_fadvise.h"
#include "mongo/util/file_allocator.h"
#include "mongo/util/options_parser/option_section.h"
#include "mongo/util/password.h"
#include "mongo/util/text.h"
#include "mongo/util/version.h"

using namespace std;
using namespace mongo;

namespace mongo {

    CmdLine cmdLine;

    moe::OptionSection options("options");
    moe::Environment _params;

    Tool::Tool( string name , string defaultDB ,
                string defaultCollection , bool usesstdout , bool quiet) :
        _name( name ) , _db( defaultDB ) , _coll( defaultCollection ) ,
        _usesstdout(usesstdout) , _quiet(quiet) , _noconnection(false) ,
        _autoreconnect(false) , _conn(0) , _slaveConn(0) , _paired(false) { }

    Tool::~Tool() {
        if ( _conn )
            delete _conn;
    }

    void Tool::printVersion(ostream &out) {
        out << _name << " version " << mongo::versionString;
        if (mongo::versionString[strlen(mongo::versionString)-1] == '-')
            out << " (commit " << mongo::gitVersion() << ")";
        out << endl;
    }
    int Tool::main( int argc , char ** argv, char ** envp ) {
        static StaticObserver staticObserver;

        setGlobalAuthorizationManager(new AuthorizationManager(new AuthzManagerExternalStateMock()));

        cmdLine.prealloc = false;

        // The default value may vary depending on compile options, but for tools
        // we want durability to be disabled.
        cmdLine.dur = false;

        _name = argv[0];

        mongo::runGlobalInitializersOrDie(argc, argv, envp);

        // Set authentication parameters
        if ( _params.count( "authenticationDatabase" ) ) {
            _authenticationDatabase = _params["authenticationDatabase"].as<string>();
        }

        if ( _params.count( "authenticationMechanism" ) ) {
            _authenticationMechanism = _params["authenticationMechanism"].as<string>();
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
            ::_exit(0);
        }

        if ( _params.count( "version" ) ) {
            printVersion(cout);
            ::_exit(0);
        }

        if (_params.count("verbose")) {
            logger::globalLogDomain()->setMinimumLoggedSeverity(logger::LogSeverity::Debug(1));
        }

        for (string s = "vv"; s.length() <= 12; s.append("v")) {
            if (_params.count(s)) {
                logger::globalLogDomain()->setMinimumLoggedSeverity(logger::LogSeverity::Debug(s.length()));
            }
        }

        if ( hasParam("quiet") ) {
            _quiet = true;
        }

#ifdef MONGO_SSL
        if (_params.count("ssl")) {
            mongo::cmdLine.sslOnNormalPorts = true;
        }
#endif

        if ( _params.count( "db" ) )
            _db = _params["db"].as<string>();

        if ( _params.count( "collection" ) )
            _coll = _params["collection"].as<string>();

        if ( _params.count( "username" ) )
            _username = _params["username"].as<string>();

        if ( _params.count( "password" ) ) {
            _password = _params["password"].as<string>();
            if ( _password.empty() ) {
                _password = askPassword();
            }
        }

        if (_params.count("ipv6"))
            enableIPv6();

        bool useDirectClient = hasParam( "dbpath" );
        if ( useDirectClient && _params.count("journal")){
            cmdLine.dur = true;
        }

        preSetup();

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
                    ::_exit(-1);
                }

                _conn = cs.connect( errmsg );
                if ( ! _conn ) {
                    cerr << "couldn't connect to [" << _host << "] " << errmsg << endl;
                    ::_exit(-1);
                }

                if (!_quiet) {
                    (_usesstdout ? cout : cerr ) << "connected to: " << _host << endl;
                }
            }

        }
        else {
            if ( _params.count( "directoryperdb" ) ) {
                directoryperdb = true;
            }
            verify( lastError.get( true ) );

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
                dbexit( EXIT_FS );
                ::_exit(EXIT_FAILURE);
            }

            FileAllocator::get()->start();

            dur::startup();
        }

        int ret = -1;
        try {
            if (!useDirectClient && !_noconnection)
                auth();
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

        fflush(stdout);
        fflush(stderr);
        ::_exit(ret);
    }

    DBClientBase& Tool::conn( bool slaveIfPaired ) {
        if ( slaveIfPaired && _conn->type() == ConnectionString::SET ) {
            if (!_slaveConn) {
                DBClientReplicaSet* rs = static_cast<DBClientReplicaSet*>(_conn);
                _slaveConn = &rs->slaveConn();
            }
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

    std::string Tool::getAuthenticationDatabase() {
        if (!_authenticationDatabase.empty()) {
            return _authenticationDatabase;
        }

        if (!_db.empty()) {
            return _db;
        }

        return "admin";
    }

    /**
     * Validate authentication on the server for the given dbname.
     */
    void Tool::auth() {

        if ( _username.empty() ) {
            // Make sure that we don't need authentication to connect to this db
            // findOne throws an AssertionException if it's not authenticated.
            if (_coll.size() > 0) {
                // BSONTools don't have a collection
                conn().findOne(getNS(), Query("{}"), 0, QueryOption_SlaveOk);
            }

            return;
        }

        _conn->auth( BSON( saslCommandUserSourceFieldName << getAuthenticationDatabase() <<
                           saslCommandUserFieldName << _username <<
                           saslCommandPasswordFieldName << _password  <<
                           saslCommandMechanismFieldName << _authenticationMechanism ) );
    }

    BSONTool::BSONTool( const char * name, bool objcheck )
        : Tool( name , "" , "" , false ) , _objcheck( objcheck ) { }


    int BSONTool::run() {
        if ( hasParam( "objcheck" ) )
            _objcheck = true;
        else if ( hasParam( "noobjcheck" ) )
            _objcheck = false;

        if ( hasParam( "filter" ) )
            _matcher.reset( new Matcher( fromjson( getParam( "filter" ) ) ) );

        return doRun();
    }

    long long BSONTool::processFile( const boost::filesystem::path& root ) {
        _fileName = root.string();

        unsigned long long fileLength = file_size( root );

        if ( fileLength == 0 ) {
            if (!_quiet) {
                (_usesstdout ? cout : cerr ) << "file " << _fileName << " empty, skipping" << endl;
            }
            return 0;
        }


        FILE* file = fopen( _fileName.c_str() , "rb" );
        if ( ! file ) {
            cerr << "error opening file: " << _fileName << " " << errnoWithDescription() << endl;
            return 0;
        }

#ifdef POSIX_FADV_SEQUENTIAL
        posix_fadvise(fileno(file), 0, fileLength, POSIX_FADV_SEQUENTIAL);
#endif

        if (!_quiet && logger::globalLogDomain()->shouldLog(logger::LogSeverity::Debug(1))) {
            (_usesstdout ? cout : cerr ) << "\t file size: " << fileLength << endl;
        }

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

            int size = ((int*)buf)[0];
            uassert( 10264 , str::stream() << "invalid object size: " << size , size < BUF_SIZE );

            amt = fread(buf+4, 1, size-4, file);
            verify( amt == (size_t)( size - 4 ) );

            BSONObj o( buf );
            if ( _objcheck && ! o.valid() ) {
                cerr << "INVALID OBJECT - going to try and print out " << endl;
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
        if (!_quiet) {
            (_usesstdout ? cout : cerr ) << m.hits() << " objects found" << endl;
            if ( _matcher.get() )
                (_usesstdout ? cout : cerr ) << processed << " objects processed" << endl;
        }
        return processed;
    }

}

#if defined(_WIN32)
// In Windows, wmain() is an alternate entry point for main(), and receives the same parameters
// as main() but encoded in Windows Unicode (UTF-16); "wide" 16-bit wchar_t characters.  The
// WindowsCommandLine object converts these wide character strings to a UTF-8 coded equivalent
// and makes them available through the argv() and envp() members.  This enables toolMain()
// to process UTF-8 encoded arguments and environment variables without regard to platform.
int wmain(int argc, wchar_t* argvW[], wchar_t* envpW[]) {
    mongo::WindowsCommandLine wcl(argc, argvW, envpW);
    auto_ptr<Tool> instance = (*Tool::createInstance)();
    int exitCode = instance->main(argc, wcl.argv(), wcl.envp());
    ::_exit(exitCode);
}

#else
int main(int argc, char* argv[], char** envp) {
    auto_ptr<Tool> instance = (*Tool::createInstance)();
    ::_exit(instance->main(argc, argv, envp));
}
#endif
