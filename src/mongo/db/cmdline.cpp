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

#include <boost/algorithm/string.hpp>

#include "mongo/pch.h"

#include "mongo/db/cmdline.h"

#include "mongo/base/status.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/server_parameters.h"
#include "mongo/logger/message_event_utf8_encoder.h"
#include "mongo/util/map_util.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/listen.h"
#include "mongo/util/password.h"

#ifdef _WIN32
#include <direct.h>
#endif

#define MAX_LINE_LENGTH 256

#include <fstream>

namespace po = boost::program_options;

namespace mongo {

#ifdef _WIN32
    string dbpath = "\\data\\db\\";
#else
    string dbpath = "/data/db/";
#endif

    static bool _isPasswordArgument(char const* argumentName);
    static bool _isPasswordSwitch(char const* switchName);

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
        ("logpath", po::value<string>() , "log file to send write to instead of stdout - has to be a file, not directory" )
        ("logappend" , "append to logpath instead of over-writing" )
        ("logTimestampFormat", po::value<string>(), "Desired format for timestamps in log "
         "messages. One of ctime, iso8601-utc or iso8601-local")
        ("pidfilepath", po::value<string>(), "full path to pidfile (if not set, no pidfile is created)")
        ("keyFile", po::value<string>(), "private key for cluster authentication")
        ("setParameter", po::value< std::vector<std::string> >()->composing(),
                "Set a configurable parameter")
        ("httpinterface", "enable http interface")
        ("clusterAuthMode", po::value<std::string>(&cmdLine.clusterAuthMode),
         "Authentication mode used for cluster authentication."
         " Alternatives are (keyfile|sendKeyfile|sendX509|x509)")
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
        ("sslClusterFile", po::value<string>(&cmdLine.sslClusterFile), 
         "Key file for internal SSL authentication" )
        ("sslClusterPassword", new PasswordValue(&cmdLine.sslClusterPassword), 
         "Internal authentication key file password" )
        ("sslCAFile", po::value<std::string>(&cmdLine.sslCAFile), 
         "Certificate Authority file for SSL")
        ("sslCRLFile", po::value<std::string>(&cmdLine.sslCRLFile),
         "Certificate Revocation List file for SSL")
        ("sslWeakCertificateValidation", "allow client to connect without presenting a certificate")
        ("sslFIPSMode", "activate FIPS 140-2 mode at startup")
#endif
        ;
        
        // Extra hidden options
        hidden.add_options()
        ("nohttpinterface", "disable http interface")
        ("objcheck", "inspect client data for validity on receipt (DEFAULT)")
        ("noobjcheck", "do NOT inspect client data for validity on receipt")
        ("traceExceptions", "log stack traces for every exception")
        ("enableExperimentalIndexStatsCmd", po::bool_switch(&cmdLine.experimental.indexStatsCmdEnabled),
                "EXPERIMENTAL (UNSUPPORTED). Enable command computing aggregate statistics on indexes.")
        ("enableExperimentalStorageDetailsCmd", po::bool_switch(&cmdLine.experimental.storageDetailsCmdEnabled),
                "EXPERIMENTAL (UNSUPPORTED). Enable command computing aggregate statistics on storage.")
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

    bool CmdLine::store( const std::vector<std::string>& argv,
                         boost::program_options::options_description& visible,
                         boost::program_options::options_description& hidden,
                         boost::program_options::positional_options_description& positional,
                         boost::program_options::variables_map &params ) {


        if (argv.empty())
            return false;

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

            po::store( po::command_line_parser(std::vector<std::string>(argv.begin() + 1,
                                                                        argv.end()))
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
            std::vector<std::string> censoredArgv = argv;
            censor(&censoredArgv);
            for (size_t i=0; i < censoredArgv.size(); i++) {
                b << censoredArgv[i];
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
                            if ( _isPasswordArgument(key.c_str()) ) {
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
            logger::globalLogDomain()->setMinimumLoggedSeverity(logger::LogSeverity::Debug(1));
        }

        for (string s = "vv"; s.length() <= 12; s.append("v")) {
            if (params.count(s)) {
                logger::globalLogDomain()->setMinimumLoggedSeverity(
                        logger::LogSeverity::Debug(s.length()));
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
        }

        if (params.count("objcheck")) {
            cmdLine.objcheck = true;
        }
        if (params.count("noobjcheck")) {
            if (params.count("objcheck")) {
                out() << "can't have both --objcheck and --noobjcheck" << endl;
                return false;
            }
            cmdLine.objcheck = false;
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

        if (params.count("logTimestampFormat")) {
            using logger::MessageEventDetailsEncoder;
            std::string formatterName = params["logTimestampFormat"].as<string>();
            if (formatterName == "ctime") {
                MessageEventDetailsEncoder::setDateFormatter(dateToCtimeString);
            }
            else if (formatterName == "iso8601-utc") {
                MessageEventDetailsEncoder::setDateFormatter(dateToISOStringUTC);
            }
            else if (formatterName == "iso8601-local") {
                MessageEventDetailsEncoder::setDateFormatter(dateToISOStringLocal);
            }
            else {
                cout << "Value of logTimestampFormat must be one of ctime, iso8601-utc or "
                    "iso8601-local; not \"" << formatterName << "\"." << endl;
                return false;
            }
        }
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

        if (params.count("setParameter")) {
            std::vector<std::string> parameters =
                params["setParameter"].as<std::vector<std::string> >();
            for (size_t i = 0, length = parameters.size(); i < length; ++i) {
                std::string name;
                std::string value;
                if (!mongoutils::str::splitOn(parameters[i], '=', name, value)) {
                    cout << "Illegal option assignment: \"" << parameters[i] << "\"" << endl;
                    return false;
                }
                ServerParameter* parameter = mapFindWithDefault(
                        ServerParameterSet::getGlobal()->getMap(),
                        name,
                        static_cast<ServerParameter*>(NULL));
                if (NULL == parameter) {
                    cout << "Illegal --setParameter parameter: \"" << name << "\"" << endl;
                    return false;
                }
                if (!parameter->allowedToChangeAtStartup()) {
                    cout << "Cannot use --setParameter to set \"" << name << "\" at startup" <<
                        endl;
                    return false;
                }
                Status status = parameter->setFromString(value);
                if (!status.isOK()) {
                    cout << "Bad value for parameter \"" << name << "\": " << status.reason()
                         << endl;
                    return false;
                }
            }
        }
        if (!params.count("clusterAuthMode")){
            cmdLine.clusterAuthMode = "keyfile";
        }

#ifdef MONGO_SSL
        if (params.count("sslWeakCertificateValidation")) {
            cmdLine.sslWeakCertificateValidation = true;
        }
        if (params.count("sslOnNormalPorts")) {
            cmdLine.sslOnNormalPorts = true;
            if ( cmdLine.sslPEMKeyFile.size() == 0 ) {
                log() << "need sslPEMKeyFile with sslOnNormalPorts" << endl;
                return false;
            }
            if (cmdLine.sslWeakCertificateValidation &&
                cmdLine.sslCAFile.empty()) {
                log() << "need sslCAFile with sslWeakCertificateValidation" << endl;
                return false;
            }
            if (!cmdLine.sslCRLFile.empty() &&
                cmdLine.sslCAFile.empty()) {
                log() << "need sslCAFile with sslCRLFile" << endl;
                return false;
            }
            if (params.count("sslFIPSMode")) {
                cmdLine.sslFIPSMode = true;
            }
        }
        else if (cmdLine.sslPEMKeyFile.size() || 
                 cmdLine.sslPEMKeyPassword.size() ||
                 cmdLine.sslClusterFile.size() ||
                 cmdLine.sslClusterPassword.size() ||
                 cmdLine.sslCAFile.size() ||
                 cmdLine.sslCRLFile.size() ||
                 cmdLine.sslWeakCertificateValidation ||
                 cmdLine.sslFIPSMode) {
            log() << "need to enable sslOnNormalPorts" << endl;
            return false;
        }
        if (cmdLine.clusterAuthMode == "sendKeyfile" || 
            cmdLine.clusterAuthMode == "sendX509" || 
            cmdLine.clusterAuthMode == "x509") {
            if (!cmdLine.sslOnNormalPorts){
                log() << "need to enable sslOnNormalPorts" << endl;
                return false;
            }
        }
        else if (cmdLine.clusterAuthMode != "keyfile") {
            log() << "unsupported value for clusterAuthMode " << cmdLine.clusterAuthMode << endl;
            return false;
        }
#else // ifdef MONGO_SSL
        // Keyfile is currently the only supported value if not using SSL 
        if (cmdLine.clusterAuthMode != "keyfile") {
            log() << "unsupported value for clusterAuthMode " << cmdLine.clusterAuthMode << endl;
            return false;
        }
#endif

        return true;
    }

    static bool _isPasswordArgument(const char* argumentName) {
        static const char* const passwordArguments[] = {
            "sslPEMKeyPassword",
            "servicePassword",
            NULL  // Last entry sentinel.
        };
        for (const char* const* current = passwordArguments; *current; ++current) {
            if (mongoutils::str::equals(argumentName, *current))
                return true;
        }
        return false;
    }

    static bool _isPasswordSwitch(const char* switchName) {
        if (switchName[0] != '-')
            return false;
        size_t i = 1;
        if (switchName[1] == '-')
            i = 2;
        switchName += i;

        return _isPasswordArgument(switchName);
    }

    static void _redact(char* arg) {
        for (; *arg; ++arg)
            *arg = 'x';
    }

    void CmdLine::censor(std::vector<std::string>* args) {
        for (size_t i = 0; i < args->size(); ++i) {
            std::string& arg = args->at(i);
            const std::string::iterator endSwitch = std::find(arg.begin(), arg.end(), '=');
            std::string switchName(arg.begin(), endSwitch);
            if (_isPasswordSwitch(switchName.c_str())) {
                if (endSwitch == arg.end()) {
                    if (i + 1 < args->size()) {
                        args->at(i + 1) = "<password>";
                    }
                }
                else {
                    arg = switchName + "=<password>";
                }
            }
        }
    }

    void CmdLine::censor(int argc, char** argv) {
        // Algorithm:  For each arg in argv:
        //   Look for an equal sign in arg; if there is one, temporarily nul it out.
        //   check to see if arg is a password switch.  If so, overwrite the value
        //     component with xs.
        //   restore the nul'd out equal sign, if any.
        for (int i = 0; i < argc; ++i) {

            char* const arg = argv[i];
            char* const firstEqSign = strchr(arg, '=');
            if (NULL != firstEqSign) {
                *firstEqSign = '\0';
            }

            if (_isPasswordSwitch(arg)) {
                if (NULL == firstEqSign) {
                    if (i + 1 < argc) {
                        _redact(argv[i + 1]);
                    }
                }
                else {
                    _redact(firstEqSign + 1);
                }
            }

            if (NULL != firstEqSign) {
                *firstEqSign = '=';
            }
        }
    }

    void printCommandLineOpts() {
        log() << "options: " << parsedOpts << endl;
    }
}
