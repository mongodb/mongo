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
#include "../util/net/listen.h"
#include "../bson/util/builder.h"
#include "mongo/util/mongoutils/str.h"
#ifdef _WIN32
#include <direct.h>
#endif
#include "globals.h"

#define MAX_LINE_LENGTH 256

#include <fstream>

namespace po = boost::program_options;

namespace mongo {

namespace {
    BSONArray argvArray;
    BSONObj parsedOpts;
}  // namespace

    BSONArray CmdLine::getArgvArray() {
        return argvArray;
    }

    BSONObj CmdLine::getParsedOpts() {
        return parsedOpts;
    }

    void CmdLine::addGlobalOptions( boost::program_options::options_description& general ,
                                    boost::program_options::options_description& hidden ,
                                    boost::program_options::options_description& ssl_options ) {
        /* support for -vv -vvvv etc. */
        for (string s = "vv"; s.length() <= 12; s.append("v")) {
            hidden.add_options()(s.c_str(), "verbose");
        }

        StringBuilder portInfoBuilder;
        StringBuilder maxConnInfoBuilder;

        portInfoBuilder << "specify port number - " << DefaultDBPort << " by default";
        maxConnInfoBuilder << "max number of simultaneous connections - " << DEFAULT_MAX_CONN << " by default";

        general.add_options()
        ("help,h", "show this usage information")
        ("version", "show version information")
        ("config,f", po::value<string>(), "configuration file specifying additional options")
        ("verbose,v", "be more verbose (include multiple times for more verbosity e.g. -vvvvv)")
        ("quiet", "quieter output")
        ("port", po::value<int>(&cmdLine.port), portInfoBuilder.str().c_str())
        ("bind_ip", po::value<string>(&cmdLine.bind_ip), "comma separated list of ip addresses to listen on - all local ips by default")
        ("maxConns",po::value<int>(), maxConnInfoBuilder.str().c_str())
        ("objcheck", "inspect client data for validity on receipt")
        ("logpath", po::value<string>() , "log file to send write to instead of stdout - has to be a file, not directory" )
        ("logappend" , "append to logpath instead of over-writing" )
        ("pidfilepath", po::value<string>(), "full path to pidfile (if not set, no pidfile is created)")
        ("keyFile", po::value<string>(), "private key for cluster authentication")
        ("enableFaultInjection", "enable the fault injection framework, for debugging."
                " DO NOT USE IN PRODUCTION")
#ifndef _WIN32
        ("nounixsocket", "disable listening on unix sockets")
        ("unixSocketPrefix", po::value<string>(), "alternative directory for UNIX domain sockets (defaults to /tmp)")
        ("fork" , "fork server process" )
        ("syslog" , "log to system's syslog facility instead of file or stdout" )
#endif
        ;
        

#ifdef MONGO_SSL
        ssl_options.add_options()
        ("sslOnNormalPorts" , "use ssl on configured ports" )
        ("sslPEMKeyFile" , po::value<string>(&cmdLine.sslPEMKeyFile), "PEM file for ssl" )
        ("sslPEMKeyPassword" , new PasswordValue(&cmdLine.sslPEMKeyPassword) , "PEM file password" )
#endif
        ;
        
        // Extra hidden options
        hidden.add_options()
        ("traceExceptions", "log stack traces for every exception");
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

        {
            BSONArrayBuilder b;
            for (int i=0; i < argc; i++) {
                b << argv[i];
                if ( mongoutils::str::equals(argv[i], "--sslPEMKeyPassword")
                     || mongoutils::str::equals(argv[i], "-sslPEMKeyPassword")
                     || mongoutils::str::equals(argv[i], "--servicePassword")
                     || mongoutils::str::equals(argv[i], "-servicePassword")) {
                    b << "<password>";
                    i++;

                    // hide password from ps output
                    char* arg = argv[i];
                    while (*arg) {
                        *arg++ = 'x';
                    }
                }
            }
            argvArray = b.arr();
        }

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
                            if ( key == "servicePassword" || key == "sslPEMKeyPassword" ) {
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

        if (params.count("traceExceptions")) {
            DBException::traceExceptions = true;
        }

        if (params.count("maxConns")) {
            cmdLine.maxConns = params["maxConns"].as<int>();

            if ( cmdLine.maxConns < 5 ) {
                out() << "maxConns has to be at least 5" << endl;
                return false;
            }
            else if ( cmdLine.maxConns > MAX_MAX_CONN ) {
                out() << "maxConns can't be greater than " << MAX_MAX_CONN << endl;
                return false;
            }
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

#ifndef _WIN32
        if (params.count("unixSocketPrefix")) {
            cmdLine.socket = params["unixSocketPrefix"].as<string>();
        }

        if (params.count("nounixsocket")) {
            cmdLine.noUnixSocket = true;
        }

        if (params.count("fork") && !params.count("shutdown")) {
            cmdLine.doFork = true;
        }
#endif  // _WIN32

        if (params.count("logpath")) {
            cmdLine.logpath = params["logpath"].as<string>();
            if (cmdLine.logpath.empty()) {
                cout << "logpath cannot be empty if supplied" << endl;
                return false;
            }
        }

        cmdLine.logWithSyslog = params.count("syslog");
        cmdLine.logAppend = params.count("logappend");
        if (!cmdLine.logpath.empty() && cmdLine.logWithSyslog) {
            cout << "Cant use both a logpath and syslog " << endl;
            return false;
        }

        if (cmdLine.doFork && cmdLine.logpath.empty() && !cmdLine.logWithSyslog) {
            cout << "--fork has to be used with --logpath or --syslog" << endl;
            return false;
        }

        if (params.count("keyFile")) {
            cmdLine.keyFile = params["keyFile"].as<string>();
        }

        if ( params.count("pidfilepath")) {
            cmdLine.pidFile = params["pidfilepath"].as<string>();
        }

#ifdef MONGO_SSL
        if (params.count("sslOnNormalPorts") ) {
            cmdLine.sslOnNormalPorts = true;
        }
#endif

        return true;
    }

    void printCommandLineOpts() {
        log() << "options: " << parsedOpts << endl;
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
