// cmdline.cpp

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

#include "pch.h"
#include "cmdline.h"
#include "commands.h"
#include "../util/password.h"
#include "../util/processinfo.h"
#include "../util/net/listen.h"
#include "security_common.h"
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#endif
#include "globals.h"

#define MAX_LINE_LENGTH 256

#include <fstream>
#include <boost/filesystem/operations.hpp>

namespace po = boost::program_options;
namespace fs = boost::filesystem;

namespace mongo {

    void setupSignals( bool inFork );
    string getHostNameCached();
    static BSONArray argvArray;
    static BSONObj parsedOpts;

    void CmdLine::addGlobalOptions( boost::program_options::options_description& general ,
                                    boost::program_options::options_description& hidden ) {
        /* support for -vv -vvvv etc. */
        for (string s = "vv"; s.length() <= 12; s.append("v")) {
            hidden.add_options()(s.c_str(), "verbose");
        }

        general.add_options()
        ("help,h", "show this usage information")
        ("version", "show version information")
        ("config,f", po::value<string>(), "configuration file specifying additional options")
        ("verbose,v", "be more verbose (include multiple times for more verbosity e.g. -vvvvv)")
        ("quiet", "quieter output")
        ("port", po::value<int>(&cmdLine.port), "specify port number")
        ("bind_ip", po::value<string>(&cmdLine.bind_ip), "comma separated list of ip addresses to listen on - all local ips by default")
        ("maxConns",po::value<int>(), "max number of simultaneous connections")
        ("objcheck", "inspect client data for validity on receipt")
        ("logpath", po::value<string>() , "log file to send write to instead of stdout - has to be a file, not directory" )
        ("logappend" , "append to logpath instead of over-writing" )
        ("pidfilepath", po::value<string>(), "full path to pidfile (if not set, no pidfile is created)")
        ("keyFile", po::value<string>(), "private key for cluster authentication (only for replica sets)")
#ifndef _WIN32
        ("nounixsocket", "disable listening on unix sockets")
        ("unixSocketPrefix", po::value<string>(), "alternative directory for UNIX domain sockets (defaults to /tmp)")
        ("fork" , "fork server process" )
        ("syslog" , "log to system's syslog facility instead of file or stdout" )
#endif
        ;
        
        hidden.add_options()
        ("cloud", po::value<string>(), "custom dynamic host naming")
#ifdef MONGO_SSL
        ("sslOnNormalPorts" , "use ssl on configured ports" )
        ("sslPEMKeyFile" , po::value<string>(&cmdLine.sslPEMKeyFile), "PEM file for ssl" )
        ("sslPEMKeyPassword" , new PasswordValue(&cmdLine.sslPEMKeyPassword) , "PEM file password" )
#endif
        ;

    }

#if defined(_WIN32)
    void CmdLine::addWindowsOptions( boost::program_options::options_description& windows ,
                                     boost::program_options::options_description& hidden ) {
        windows.add_options()
        ("install", "install Windows service")
        ("remove", "remove Windows service")
        ("reinstall", "reinstall Windows service (equivalent to --remove followed by --install)")
        ("serviceName", po::value<string>(), "Windows service name")
        ("serviceDisplayName", po::value<string>(), "Windows service display name")
        ("serviceDescription", po::value<string>(), "Windows service description")
        ("serviceUser", po::value<string>(), "account for service execution")
        ("servicePassword", po::value<string>(), "password used to authenticate serviceUser")
        ;
        hidden.add_options()("service", "start mongodb service");
    }
#endif

    void CmdLine::parseConfigFile( istream &f, stringstream &ss ) {
        string s;
        char line[MAX_LINE_LENGTH];

        while ( f ) {
            f.getline(line, MAX_LINE_LENGTH);
            s = line;
            std::remove(s.begin(), s.end(), ' ');
            std::remove(s.begin(), s.end(), '\t');
            boost::to_upper(s);

            if ( s.find( "FASTSYNC" ) != string::npos )
                cout << "warning \"fastsync\" should not be put in your configuration file" << endl;

            if ( s.c_str()[0] == '#' ) { 
                // skipping commented line
            } else if ( s.find( "=FALSE" ) == string::npos ) {
                ss << line << endl;
            } else {
                cout << "warning: remove or comment out this line by starting it with \'#\', skipping now : " << line << endl;
            }
        }
        return;
    }

#ifndef _WIN32
    // support for exit value propagation with fork
    void launchSignal( int sig ) {
        if ( sig == SIGUSR2 ) {
            pid_t cur = getpid();
            
            if ( cur == cmdLine.parentProc || cur == cmdLine.leaderProc ) {
                // signal indicates successful start allowing us to exit
                _exit(0);
            } 
        }
    }

    void setupLaunchSignals() {
        verify( signal(SIGUSR2 , launchSignal ) != SIG_ERR );
    }


    void CmdLine::launchOk() {
        if ( cmdLine.doFork ) {
            // killing leader will propagate to parent
            verify( kill( cmdLine.leaderProc, SIGUSR2 ) == 0 );
        }
    }
#endif

    bool CmdLine::store( int argc , char ** argv ,
                         boost::program_options::options_description& visible,
                         boost::program_options::options_description& hidden,
                         boost::program_options::positional_options_description& positional,
                         boost::program_options::variables_map &params ) {


        {
            // setup binary name
            cmdLine.binaryName = argv[0];
            size_t i = cmdLine.binaryName.rfind( '/' );
            if ( i != string::npos )
                cmdLine.binaryName = cmdLine.binaryName.substr( i + 1 );
            
            // setup cwd
            char buffer[1024];
#ifdef _WIN32
            verify( _getcwd( buffer , 1000 ) );
#else
            verify( getcwd( buffer , 1000 ) );
#endif
            cmdLine.cwd = buffer;
        }
        

        /* don't allow guessing - creates ambiguities when some options are
         * prefixes of others. allow long disguises and don't allow guessing
         * to get away with our vvvvvvv trick. */
        int style = (((po::command_line_style::unix_style ^
                       po::command_line_style::allow_guessing) |
                      po::command_line_style::allow_long_disguise) ^
                     po::command_line_style::allow_sticky);


        try {

            po::options_description all;
            all.add( visible );
            all.add( hidden );

            po::store( po::command_line_parser(argc, argv)
                       .options( all )
                       .positional( positional )
                       .style( style )
                       .run(),
                       params );

            if ( params.count("config") ) {
                ifstream f( params["config"].as<string>().c_str() );
                if ( ! f.is_open() ) {
                    cout << "ERROR: could not read from config file" << endl << endl;
                    cout << visible << endl;
                    return false;
                }

                stringstream ss;
                CmdLine::parseConfigFile( f, ss );
                po::store( po::parse_config_file( ss , all ) , params );
                f.close();
            }

            po::notify(params);
        }
        catch (po::error &e) {
            cout << "error command line: " << e.what() << endl;
            cout << "use --help for help" << endl;
            //cout << visible << endl;
            return false;
        }

        if (params.count("verbose")) {
            logLevel = 1;
        }

        for (string s = "vv"; s.length() <= 12; s.append("v")) {
            if (params.count(s)) {
                logLevel = s.length();
            }
        }

        if (params.count("quiet")) {
            cmdLine.quiet = true;
        }

        if ( params.count( "maxConns" ) ) {
            int newSize = params["maxConns"].as<int>();
            if ( newSize < 5 ) {
                out() << "maxConns has to be at least 5" << endl;
                ::exit( EXIT_BADOPTIONS );
            }
            else if ( newSize >= 10000000 ) {
                out() << "maxConns can't be greater than 10000000" << endl;
                ::exit( EXIT_BADOPTIONS );
            }
            connTicketHolder.resize( newSize );
        }

        if (params.count("objcheck")) {
            cmdLine.objcheck = true;
        }

        if (params.count("bind_ip")) {
            // passing in wildcard is the same as default behavior; remove and warn
            if ( cmdLine.bind_ip ==  "0.0.0.0" ) {
                cout << "warning: bind_ip of 0.0.0.0 is unnecessary; listens on all ips by default" << endl;
                cmdLine.bind_ip = "";
            }
        }

        string logpath;

#ifndef _WIN32
        if (params.count("unixSocketPrefix")) {
            cmdLine.socket = params["unixSocketPrefix"].as<string>();
            if (!fs::is_directory(cmdLine.socket)) {
                cout << cmdLine.socket << " must be a directory" << endl;
                ::exit(-1);
            }
        }

        if (params.count("nounixsocket")) {
            cmdLine.noUnixSocket = true;
        }

        if (params.count("fork") && !params.count("shutdown")) {
            cmdLine.doFork = true;
            if ( ! params.count( "logpath" ) && ! params.count( "syslog" ) ) {
                cout << "--fork has to be used with --logpath or --syslog" << endl;
                ::exit(EXIT_BADOPTIONS);
            }

            if ( params.count( "logpath" ) ) {
                // test logpath
                logpath = params["logpath"].as<string>();
                verify( logpath.size() );
                if ( logpath[0] != '/' ) {
                    logpath = cmdLine.cwd + "/" + logpath;
                }
                bool exists = boost::filesystem::exists( logpath );
                FILE * test = fopen( logpath.c_str() , "a" );
                if ( ! test ) {
                    cout << "can't open [" << logpath << "] for log file: " << errnoWithDescription() << endl;
                    ::exit(-1);
                }
                fclose( test );
                // if we created a file, unlink it (to avoid confusing log rotation code)
                if ( ! exists ) {
                    unlink( logpath.c_str() );
                }
            }

            cout.flush();
            cerr.flush();
            
            cmdLine.parentProc = getpid();
            
            // facilitate clean exit when child starts successfully
            setupLaunchSignals();

            pid_t c = fork();
            if ( c ) {
                int pstat;
                waitpid(c, &pstat, 0);

                if ( WIFEXITED(pstat) ) {
                    if ( ! WEXITSTATUS(pstat) ) {
                        cout << "child process started successfully, parent exiting" << endl;
                    }

                    _exit( WEXITSTATUS(pstat) );
                }

                _exit(50);
            }

            if ( chdir("/") < 0 ) {
                cout << "Cant chdir() while forking server process: " << strerror(errno) << endl;
                ::exit(-1);
            }
            setsid();
            
            cmdLine.leaderProc = getpid();

            pid_t c2 = fork();
            if ( c2 ) {
                int pstat;
                cout << "forked process: " << c2 << endl;
                waitpid(c2, &pstat, 0);

                if ( WIFEXITED(pstat) ) {
                    _exit( WEXITSTATUS(pstat) );
                }

                _exit(51);
            }

            // stdout handled in initLogging
            //fclose(stdout);
            //freopen("/dev/null", "w", stdout);

            fclose(stderr);
            fclose(stdin);

            FILE* f = freopen("/dev/null", "w", stderr);
            if ( f == NULL ) {
                cout << "Cant reassign stderr while forking server process: " << strerror(errno) << endl;
                ::exit(-1);
            }

            f = freopen("/dev/null", "r", stdin);
            if ( f == NULL ) {
                cout << "Cant reassign stdin while forking server process: " << strerror(errno) << endl;
                ::exit(-1);
            }

            setupCoreSignals();
            setupSignals( true );
        }
        
        if (params.count("syslog")) {
            StringBuilder sb;
            sb << cmdLine.binaryName << "." << cmdLine.port;
            Logstream::useSyslog( sb.str().c_str() );
        }
#endif
        if (params.count("logpath") && !params.count("shutdown")) {
            if ( params.count("syslog") ) {
                cout << "Cant use both a logpath and syslog " << endl;
                ::exit(EXIT_BADOPTIONS);
            }
            
            if ( logpath.size() == 0 )
                logpath = params["logpath"].as<string>();
            uassert( 10033 ,  "logpath has to be non-zero" , logpath.size() );
            initLogging( logpath , params.count( "logappend" ) );
        }

        if ( params.count("pidfilepath")) {
            writePidFile( params["pidfilepath"].as<string>() );
        }

        if (params.count("keyFile")) {
            const string f = params["keyFile"].as<string>();

            if (!setUpSecurityKey(f)) {
                // error message printed in setUpPrivateKey
                ::exit(EXIT_BADOPTIONS);
            }

            cmdLine.keyFile = true;
            noauth = false;
        }
        else {
            cmdLine.keyFile = false;
        }

#ifdef MONGO_SSL
        if (params.count("sslOnNormalPorts") ) {
            cmdLine.sslOnNormalPorts = true;

            if ( cmdLine.sslPEMKeyPassword.size() == 0 ) {
                log() << "need sslPEMKeyPassword" << endl;
                ::exit(EXIT_BADOPTIONS);
            }
            
            if ( cmdLine.sslPEMKeyFile.size() == 0 ) {
                log() << "need sslPEMKeyFile" << endl;
                ::exit(EXIT_BADOPTIONS);
            }
            
            cmdLine.sslServerManager = new SSLManager( false );
            cmdLine.sslServerManager->setupPEM( cmdLine.sslPEMKeyFile , cmdLine.sslPEMKeyPassword );
        }
        else if ( cmdLine.sslPEMKeyFile.size() || cmdLine.sslPEMKeyPassword.size() ) {
            log() << "need to enable sslOnNormalPorts" << endl;
            ::exit(EXIT_BADOPTIONS);
        }
#endif
        
        {
            BSONObjBuilder b;
            for (po::variables_map::const_iterator it(params.begin()), end(params.end()); it != end; it++){
                if (!it->second.defaulted()){
                    const string& key = it->first;
                    const po::variable_value& value = it->second;
                    const type_info& type = value.value().type();

                    if (type == typeid(string)){
                        if (value.as<string>().empty())
                            b.appendBool(key, true); // boost po uses empty string for flags like --quiet
                        else {
                            if ( key == "servicePassword" ) {
                                b.append( key, "<password>" );
                            }
                            else {
                                b.append( key, value.as<string>() );
                            }
                        }
                    }
                    else if (type == typeid(int))
                        b.append(key, value.as<int>());
                    else if (type == typeid(double))
                        b.append(key, value.as<double>());
                    else if (type == typeid(bool))
                        b.appendBool(key, value.as<bool>());
                    else if (type == typeid(long))
                        b.appendNumber(key, (long long)value.as<long>());
                    else if (type == typeid(unsigned))
                        b.appendNumber(key, (long long)value.as<unsigned>());
                    else if (type == typeid(unsigned long long))
                        b.appendNumber(key, (long long)value.as<unsigned long long>());
                    else if (type == typeid(vector<string>))
                        b.append(key, value.as<vector<string> >());
                    else
                        b.append(key, "UNKNOWN TYPE: " + demangleName(type));
                }
            }
            parsedOpts = b.obj();
        }

        {
            BSONArrayBuilder b;
            for (int i=0; i < argc; i++)
                b << argv[i];
            argvArray = b.arr();
        }

        return true;
    }

    void printCommandLineOpts() {
        log() << "options: " << parsedOpts << endl;
    }

    void ignoreSignal( int sig ) {}

    void setupCoreSignals() {
#if !defined(_WIN32)
        verify( signal(SIGUSR1 , rotateLogs ) != SIG_ERR );
        verify( signal(SIGHUP , ignoreSignal ) != SIG_ERR );
#endif
    }

    class CmdGetCmdLineOpts : Command {
    public:
        CmdGetCmdLineOpts(): Command("getCmdLineOpts") {}
        void help(stringstream& h) const { h << "get argv"; }
        virtual LockType locktype() const { return NONE; }
        virtual bool adminOnly() const { return true; }
        virtual bool slaveOk() const { return true; }

        virtual bool run(const string&, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            result.append("argv", argvArray);
            result.append("parsed", parsedOpts);
            return true;
        }

    } cmdGetCmdLineOpts;

    string prettyHostName() {
        StringBuilder s;
        s << getHostNameCached();
        if( cmdLine.port != CmdLine::DefaultDBPort )
            s << ':' << mongo::cmdLine.port;
        return s.str();
    }

    casi< map<string,ParameterValidator*> * > pv_all (NULL);

    ParameterValidator::ParameterValidator( const string& name ) : _name( name ) {
        if ( ! pv_all)
            pv_all.ref() = new map<string,ParameterValidator*>();
        (*pv_all.ref())[_name] = this;
    }
    
    ParameterValidator * ParameterValidator::get( const string& name ) {
        map<string,ParameterValidator*>::const_iterator i = pv_all.get()->find( name );
        if ( i == pv_all.get()->end() )
            return NULL;
        return i->second;
    }

}
