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
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
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
#include "mongo/util/options_parser/environment.h"
#include "mongo/util/options_parser/option_section.h"
#include "mongo/util/options_parser/options_parser.h"
#include "mongo/util/password.h"

#ifdef _WIN32
#include <direct.h>
#endif

#define MAX_LINE_LENGTH 256

#include <fstream>

namespace mongo {

    namespace moe = mongo::optionenvironment;

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

    Status CmdLine::setupBinaryName(const std::vector<std::string>& argv) {

        if (argv.empty()) {
            return Status(ErrorCodes::InternalError, "Cannot get binary name: argv array is empty");
        }

        // setup binary name
        cmdLine.binaryName = argv[0];
        size_t i = cmdLine.binaryName.rfind( '/' );
        if ( i != string::npos ) {
            cmdLine.binaryName = cmdLine.binaryName.substr( i + 1 );
        }
        return Status::OK();
    }

    Status CmdLine::setupCwd() {
            // setup cwd
        char buffer[1024];
#ifdef _WIN32
        verify( _getcwd( buffer , 1000 ) );
#else
        verify( getcwd( buffer , 1000 ) );
#endif
        cmdLine.cwd = buffer;
        return Status::OK();
    }

    Status CmdLine::setArgvArray(const std::vector<std::string>& argv) {
        BSONArrayBuilder b;
        std::vector<std::string> censoredArgv = argv;
        censor(&censoredArgv);
        for (size_t i=0; i < censoredArgv.size(); i++) {
            b << censoredArgv[i];
        }
        argvArray = b.arr();
        return Status::OK();
    }

namespace {

    // Converts a map of values with dotted key names to a BSONObj with sub objects.
    // 1. Check for dotted field names and call valueMapToBSON recursively.
    // 2. Append the actual value to our builder if we did not find a dot in our key name.
    Status valueMapToBSON(const std::map<moe::Key, moe::Value>& params,
                          BSONObjBuilder* builder,
                          const std::string& prefix = std::string()) {
        for (std::map<moe::Key, moe::Value>::const_iterator it(params.begin());
                it != params.end(); it++) {
            moe::Key key = it->first;
            moe::Value value = it->second;

            // 1. Check for dotted field names and call valueMapToBSON recursively.
            // NOTE: this code depends on the fact that std::map is sorted
            //
            // EXAMPLE:
            // The map:
            // {
            //     "var1.dotted1" : false,
            //     "var2" : true,
            //     "var1.dotted2" : 6
            // }
            //
            // Gets sorted by keys as:
            // {
            //     "var1.dotted1" : false,
            //     "var1.dotted2" : 6,
            //     "var2" : true
            // }
            //
            // Which means when we see the "var1" prefix, we can iterate until we see either a name
            // without a dot or without "var1" as a prefix, aggregating its fields in a new map as
            // we go.  Because the map is sorted, once we see a name without a dot or a "var1"
            // prefix we know that we've seen everything with "var1" as a prefix and can recursively
            // build the entire sub object at once using our new map (which is the only way to make
            // a single coherent BSON sub object using this append only builder).
            //
            // The result of this function for this example should be a BSON object of the form:
            // {
            //     "var1" : {
            //         "dotted1" : false,
            //         "dotted2" : 6
            //     },
            //     "var2" : true
            // }

            // Check to see if this key name is dotted
            std::string::size_type dotOffset = key.find('.');
            if (dotOffset != string::npos) {

                // Get the name of the "section" that we are currently iterating.  This will be
                // the name of our sub object.
                std::string sectionName = key.substr(0, dotOffset);

                // Build a map of the "section" that we are iterating to be passed in a
                // recursive call.
                std::map<moe::Key, moe::Value> sectionMap;

                std::string beforeDot = key.substr(0, dotOffset);
                std::string afterDot = key.substr(dotOffset + 1, key.size() - dotOffset - 1);
                std::map<moe::Key, moe::Value>::const_iterator it_next = it;

                do {
                    // Here we know that the key at it_next has a dot and has the prefix we are
                    // currently creating a sub object for.  Since that means we will definitely
                    // process that element in this loop, advance the outer for loop iterator here.
                    it = it_next;

                    // Add the value to our section map with a key of whatever is after the dot
                    // since the section name itself will be part of our sub object builder.
                    sectionMap[afterDot] = value;

                    // Peek at the next value for our iterator and check to see if we've finished.
                    if (++it_next == params.end()) {
                        break;
                    }
                    key = it_next->first;
                    value = it_next->second;

                    // Look for a dot for our next iteration.
                    dotOffset = key.find('.');

                    beforeDot = key.substr(0, dotOffset);
                    afterDot = key.substr(dotOffset + 1, key.size() - dotOffset - 1);
                }
                while (dotOffset != string::npos && beforeDot == sectionName);

                // Use the section name in our object builder, and recursively call
                // valueMapToBSON with our sub map with keys that have the section name removed.
                BSONObjBuilder sectionObjBuilder(builder->subobjStart(sectionName));
                valueMapToBSON(sectionMap, &sectionObjBuilder, sectionName);
                sectionObjBuilder.done();

                // Our iterator is currently on the last field that matched our dot and prefix, so
                // continue to the next loop iteration.
                continue;
            }

            // 2. Append the actual value to our builder if we did not find a dot in our key name.
            const type_info& type = value.type();

            if (type == typeid(string)){
                if (value.as<string>().empty()) {
                    // boost po uses empty string for flags like --quiet
                    // TODO: Remove this when we remove boost::program_options
                    builder->appendBool(key, true);
                }
                else {
                    if ( _isPasswordArgument(key.c_str()) ) {
                        builder->append( key, "<password>" );
                    }
                    else {
                        builder->append( key, value.as<string>() );
                    }
                }
            }
            else if (type == typeid(int))
                builder->append(key, value.as<int>());
            else if (type == typeid(double))
                builder->append(key, value.as<double>());
            else if (type == typeid(bool))
                builder->appendBool(key, value.as<bool>());
            else if (type == typeid(long))
                builder->appendNumber(key, (long long)value.as<long>());
            else if (type == typeid(unsigned))
                builder->appendNumber(key, (long long)value.as<unsigned>());
            else if (type == typeid(unsigned long long))
                builder->appendNumber(key, (long long)value.as<unsigned long long>());
            else if (type == typeid(vector<string>))
                builder->append(key, value.as<vector<string> >());
            else
                builder->append(key, "UNKNOWN TYPE: " + demangleName(type));
        }
        return Status::OK();
    }
} // namespace

    Status CmdLine::setParsedOpts(moe::Environment& params) {
        const std::map<moe::Key, moe::Value> paramsMap = params.getExplicitlySet();
        BSONObjBuilder builder;
        Status ret = valueMapToBSON(paramsMap, &builder);
        if (!ret.isOK()) {
            return ret;
        }
        parsedOpts = builder.obj();
        return Status::OK();
    }

    Status CmdLine::store( const std::vector<std::string>& argv,
                           moe::OptionSection& options,
                           moe::Environment& params ) {

        Status ret = CmdLine::setupBinaryName(argv);
        if (!ret.isOK()) {
            return ret;
        }

        ret = CmdLine::setupCwd();
        if (!ret.isOK()) {
            return ret;
        }

        moe::OptionsParser parser;

        // XXX: env is not used in the parser at this point, so just leave it empty
        std::map<std::string, std::string> env;

        ret = parser.run(options, argv, env, &params);
        if (!ret.isOK()) {
            std::cerr << "Error parsing command line: " << ret.toString() << std::endl;
            std::cerr << "use --help for help" << std::endl;
            return ret;
        }

        ret = CmdLine::setArgvArray(argv);
        if (!ret.isOK()) {
            return ret;
        }

        ret = CmdLine::setParsedOpts(params);
        if (!ret.isOK()) {
            return ret;
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

        if (params.count("enableExperimentalIndexStatsCmd")) {
            cmdLine.experimental.indexStatsCmdEnabled = true;
        }
        if (params.count("enableExperimentalStorageDetailsCmd")) {
            cmdLine.experimental.storageDetailsCmdEnabled = true;
        }

        if (params.count("port")) {
            cmdLine.port = params["port"].as<int>();
        }

        if (params.count("bind_ip")) {
            cmdLine.bind_ip = params["bind_ip"].as<std::string>();
        }

        if (params.count("clusterAuthMode")) {
            cmdLine.clusterAuthMode = params["clusterAuthMode"].as<std::string>();
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
                return Status(ErrorCodes::BadValue, "maxConns has to be at least 5");
            }
        }

        if (params.count("objcheck")) {
            cmdLine.objcheck = true;
        }
        if (params.count("noobjcheck")) {
            if (params.count("objcheck")) {
                return Status(ErrorCodes::BadValue, "can't have both --objcheck and --noobjcheck");
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
                StringBuilder sb;
                sb << "Value of logTimestampFormat must be one of ctime, iso8601-utc " <<
                      "or iso8601-local; not \"" << formatterName << "\".";
                return Status(ErrorCodes::BadValue, sb.str());
            }
        }
        if (params.count("logpath")) {
            cmdLine.logpath = params["logpath"].as<string>();
            if (cmdLine.logpath.empty()) {
                return Status(ErrorCodes::BadValue, "logpath cannot be empty if supplied");
            }
        }

        cmdLine.logWithSyslog = params.count("syslog");
        cmdLine.logAppend = params.count("logappend");
        if (!cmdLine.logpath.empty() && cmdLine.logWithSyslog) {
            return Status(ErrorCodes::BadValue, "Cant use both a logpath and syslog ");
        }

        if (cmdLine.doFork && cmdLine.logpath.empty() && !cmdLine.logWithSyslog) {
            return Status(ErrorCodes::BadValue, "--fork has to be used with --logpath or --syslog");
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
                    StringBuilder sb;
                    sb << "Illegal option assignment: \"" << parameters[i] << "\"";
                    return Status(ErrorCodes::BadValue, sb.str());
                }
                ServerParameter* parameter = mapFindWithDefault(
                        ServerParameterSet::getGlobal()->getMap(),
                        name,
                        static_cast<ServerParameter*>(NULL));
                if (NULL == parameter) {
                    StringBuilder sb;
                    sb << "Illegal --setParameter parameter: \"" << name << "\"";
                    return Status(ErrorCodes::BadValue, sb.str());
                }
                if (!parameter->allowedToChangeAtStartup()) {
                    StringBuilder sb;
                    sb << "Cannot use --setParameter to set \"" << name << "\" at startup";
                    return Status(ErrorCodes::BadValue, sb.str());
                }
                Status status = parameter->setFromString(value);
                if (!status.isOK()) {
                    StringBuilder sb;
                    sb << "Bad value for parameter \"" << name << "\": " << status.reason();
                    return Status(ErrorCodes::BadValue, sb.str());
                }
            }
        }
        if (!params.count("clusterAuthMode") && params.count("keyFile")){
            cmdLine.clusterAuthMode = "keyfile";
        }

#ifdef MONGO_SSL

        if (params.count("ssl.PEMKeyFile")) {
            cmdLine.sslPEMKeyFile = params["ssl.PEMKeyFile"].as<string>();
        }

        if (params.count("ssl.PEMKeyPassword")) {
            cmdLine.sslPEMKeyPassword = params["ssl.PEMKeyPassword"].as<string>();
        }

        if (params.count("ssl.clusterFile")) {
            cmdLine.sslClusterFile = params["ssl.clusterFile"].as<string>();
        }

        if (params.count("ssl.clusterPassword")) {
            cmdLine.sslClusterPassword = params["ssl.clusterPassword"].as<string>();
        }

        if (params.count("ssl.CAFile")) {
            cmdLine.sslCAFile = params["ssl.CAFile"].as<std::string>();
        }

        if (params.count("ssl.CRLFile")) {
            cmdLine.sslCRLFile = params["ssl.CRLFile"].as<std::string>();
        }

        if (params.count("ssl.weakCertificateValidation")) {
            cmdLine.sslWeakCertificateValidation = true;
        }
        if (params.count("ssl.sslOnNormalPorts")) {
            cmdLine.sslOnNormalPorts = true;
            if ( cmdLine.sslPEMKeyFile.size() == 0 ) {
                return Status(ErrorCodes::BadValue,
                              "need sslPEMKeyFile with sslOnNormalPorts");
            }
            if (cmdLine.sslWeakCertificateValidation &&
                cmdLine.sslCAFile.empty()) {
                return Status(ErrorCodes::BadValue,
                              "need sslCAFile with sslWeakCertificateValidation");
            }
            if (!cmdLine.sslCRLFile.empty() &&
                cmdLine.sslCAFile.empty()) {
                return Status(ErrorCodes::BadValue, "need sslCAFile with sslCRLFile");
            }
            if (params.count("ssl.FIPSMode")) {
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
            return Status(ErrorCodes::BadValue, "need to enable sslOnNormalPorts");
        }
        if (cmdLine.clusterAuthMode == "sendKeyfile" || 
            cmdLine.clusterAuthMode == "sendX509" || 
            cmdLine.clusterAuthMode == "x509") {
            if (!cmdLine.sslOnNormalPorts){
                return Status(ErrorCodes::BadValue, "need to enable sslOnNormalPorts");
            }
        }
        else if (params.count("clusterAuthMode") && cmdLine.clusterAuthMode != "keyfile") {
            StringBuilder sb;
            sb << "unsupported value for clusterAuthMode " << cmdLine.clusterAuthMode;
            return Status(ErrorCodes::BadValue, sb.str());
        }
#else // ifdef MONGO_SSL
        // Keyfile is currently the only supported value if not using SSL 
        if (params.count("clusterAuthMode") && cmdLine.clusterAuthMode != "keyfile") {
            StringBuilder sb;
            sb << "unsupported value for clusterAuthMode " << cmdLine.clusterAuthMode;
            return Status(ErrorCodes::BadValue, sb.str());
        }
#endif

        return Status::OK();
    }

    static bool _isPasswordArgument(const char* argumentName) {
        static const char* const passwordArguments[] = {
            "sslPEMKeyPassword",
            "ssl.PEMKeyPassword",
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
