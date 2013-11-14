/*
 *    Copyright (C) 2013 10gen Inc.
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

#include "mongo/db/server_options_helpers.h"

#ifdef _WIN32
#include <direct.h>
#else
#define SYSLOG_NAMES
#include <syslog.h>
#endif
#include <boost/filesystem.hpp>

#include "mongo/base/status.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_parameters.h"
#include "mongo/logger/message_event_utf8_encoder.h"
#include "mongo/util/cmdline_utils/censor_cmdline.h"
#include "mongo/util/map_util.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/listen.h" // For DEFAULT_MAX_CONN
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/options_parser/startup_options.h"

namespace mongo {

/*
 * SERVER-11160 syslog.h does not define facilitynames under solaris.
 * syslog.h exports preprocessor macro INTERNAL_NOPRI if
 * facilitynames is provided. This will be used to determine
 * if facilitynames should be defined here.
 * These could also go into a syslog.h compatibility header.
 * We are using INTERNAL_NOPRI as the indicator macro for facilitynames
 * because it's defined alongside facilitynames in the syslog.h headers
 * that support SYSLOG_NAMES.
 */

namespace {

#if defined(SYSLOG_NAMES)
#if !defined(INTERNAL_NOPRI)

    typedef struct _code {
        const char* c_name;
        int         c_val;
    } CODE;

    CODE facilitynames[] =
      {
        { "auth", LOG_AUTH },
        { "cron", LOG_CRON },
        { "daemon", LOG_DAEMON },
        { "kern", LOG_KERN },
        { "lpr", LOG_LPR },
        { "mail", LOG_MAIL },
        { "news", LOG_NEWS },
        { "security", LOG_AUTH },   /* DEPRECATED */
        { "syslog", LOG_SYSLOG },
        { "user", LOG_USER },
        { "uucp", LOG_UUCP },
        { "local0", LOG_LOCAL0 },
        { "local1", LOG_LOCAL1 },
        { "local2", LOG_LOCAL2 },
        { "local3", LOG_LOCAL3 },
        { "local4", LOG_LOCAL4 },
        { "local5", LOG_LOCAL5 },
        { "local6", LOG_LOCAL6 },
        { "local7", LOG_LOCAL7 },
        { NULL, -1 }
    };

#endif // !defined(INTERNAL_NOPRI)
#endif // defined(SYSLOG_NAMES)

} // namespace

    Status addGeneralServerOptions(moe::OptionSection* options) {
        StringBuilder portInfoBuilder;
        StringBuilder maxConnInfoBuilder;

        portInfoBuilder << "specify port number - " << ServerGlobalParams::DefaultDBPort << " by default";
        maxConnInfoBuilder << "max number of simultaneous connections - "
                           << DEFAULT_MAX_CONN << " by default";

        options->addOptionChaining("help", "help,h", moe::Switch, "show this usage information");

        options->addOptionChaining("version", "version", moe::Switch, "show version information");

        options->addOptionChaining("config", "config,f", moe::String,
                "configuration file specifying additional options");

        // The verbosity level can be set at startup in the following ways.  Note that if multiple
        // methods for setting the verbosity are specified simultaneously, the verbosity will be set
        // based on the whichever option specifies the highest level
        //
        // Command Line Option | Resulting Verbosity
        // _________________________________________
        // (none)              | 0
        // --verbose ""        | 0
        // --verbose           | 1
        // --verbose v         | 1
        // --verbose vv        | 2 (etc.)
        // -v                  | 1
        // -vv                 | 2 (etc.)
        //
        // INI Config Option   | Resulting Verbosity
        // _________________________________________
        // verbose=            | 0
        // verbose=v           | 1
        // verbose=vv          | 2 (etc.)
        // v=true              | 1
        // vv=true             | 2 (etc.)
        //
        // JSON Config Option  | Resulting Verbosity
        // _________________________________________
        // { "verbose" : "" }  | 0
        // { "verbose" : "v" } | 1
        // { "verbose" : "vv" }| 2 (etc.)
        // { "v" : true }      | 1
        // { "vv" : true }     | 2 (etc.)
        options->addOptionChaining("verbose", "verbose,v", moe::String,
                "be more verbose (include multiple times for more verbosity e.g. -vvvvv)")
                                  .setImplicit(moe::Value(std::string("v")));

        options->addOptionChaining("quiet", "quiet", moe::Switch, "quieter output");

        options->addOptionChaining("port", "port", moe::Int, portInfoBuilder.str().c_str());

        options->addOptionChaining("bind_ip", "bind_ip", moe::String,
                "comma separated list of ip addresses to listen on - all local ips by default");

        options->addOptionChaining("maxConns", "maxConns", moe::Int,
                maxConnInfoBuilder.str().c_str());

        options->addOptionChaining("logpath", "logpath", moe::String,
                "log file to send write to instead of stdout - has to be a file, not directory");

#ifndef _WIN32
        options->addOptionChaining("syslogFacility", "syslogFacility", moe::String,
                "syslog facility used for monogdb syslog message");

#endif // _WIN32
        options->addOptionChaining("logappend", "logappend", moe::Switch,
                "append to logpath instead of over-writing");

        options->addOptionChaining("logTimestampFormat", "logTimestampFormat", moe::String,
                "Desired format for timestamps in log messages. One of ctime, "
                "iso8601-utc or iso8601-local");

        options->addOptionChaining("pidfilepath", "pidfilepath", moe::String,
                "full path to pidfile (if not set, no pidfile is created)");

        options->addOptionChaining("keyFile", "keyFile", moe::String,
                "private key for cluster authentication");

        options->addOptionChaining("setParameter", "setParameter", moe::StringVector,
                "Set a configurable parameter")
                                  .composing();

        options->addOptionChaining("httpinterface", "httpinterface", moe::Switch,
                "enable http interface");

        options->addOptionChaining("clusterAuthMode", "clusterAuthMode", moe::String,
                "Authentication mode used for cluster authentication. Alternatives are "
                "(keyFile|sendKeyFile|sendX509|x509)");

#ifndef _WIN32
        options->addOptionChaining("nounixsocket", "nounixsocket", moe::Switch,
                "disable listening on unix sockets");

        options->addOptionChaining("unixSocketPrefix", "unixSocketPrefix", moe::String,
                "alternative directory for UNIX domain sockets (defaults to /tmp)");

        options->addOptionChaining("fork", "fork", moe::Switch, "fork server process");

        options->addOptionChaining("syslog", "syslog", moe::Switch,
                "log to system's syslog facility instead of file or stdout");

#endif

        /* support for -vv -vvvv etc. */
        for (string s = "vv"; s.length() <= 12; s.append("v")) {
            options->addOptionChaining(s.c_str(), s.c_str(), moe::Switch, "verbose")
                                      .hidden();
        }

        // Extra hidden options
        options->addOptionChaining("nohttpinterface", "nohttpinterface", moe::Switch,
                "disable http interface")
                                  .hidden();

        options->addOptionChaining("objcheck", "objcheck", moe::Switch,
                "inspect client data for validity on receipt (DEFAULT)")
                                  .hidden();

        options->addOptionChaining("noobjcheck", "noobjcheck", moe::Switch,
                "do NOT inspect client data for validity on receipt")
                                  .hidden();

        options->addOptionChaining("traceExceptions", "traceExceptions", moe::Switch,
                "log stack traces for every exception")
                                  .hidden();

        options->addOptionChaining("enableExperimentalIndexStatsCmd",
                "enableExperimentalIndexStatsCmd", moe::Switch, "EXPERIMENTAL (UNSUPPORTED). "
                "Enable command computing aggregate statistics on indexes.")
                                  .hidden();

        options->addOptionChaining("enableExperimentalStorageDetailsCmd",
                "enableExperimentalStorageDetailsCmd", moe::Switch, "EXPERIMENTAL (UNSUPPORTED). "
                "Enable command computing aggregate statistics on storage.")
                                  .hidden();


        return Status::OK();
    }

    Status addWindowsServerOptions(moe::OptionSection* options) {
        options->addOptionChaining("install", "install", moe::Switch, "install Windows service");

        options->addOptionChaining("remove", "remove", moe::Switch, "remove Windows service");

        options->addOptionChaining("reinstall", "reinstall", moe::Switch,
                "reinstall Windows service (equivalent to --remove followed by --install)");

        options->addOptionChaining("serviceName", "serviceName", moe::String,
                "Windows service name");

        options->addOptionChaining("serviceDisplayName", "serviceDisplayName", moe::String,
                "Windows service display name");

        options->addOptionChaining("serviceDescription", "serviceDescription", moe::String,
                "Windows service description");

        options->addOptionChaining("serviceUser", "serviceUser", moe::String,
                "account for service execution");

        options->addOptionChaining("servicePassword", "servicePassword", moe::String,
                "password used to authenticate serviceUser");


        options->addOptionChaining("service", "service", moe::Switch, "start mongodb service")
                                  .hidden();


        return Status::OK();
    }

    namespace {
        // Helpers for option storage
        Status setupBinaryName(const std::vector<std::string>& argv) {

            if (argv.empty()) {
                return Status(ErrorCodes::InternalError, "Cannot get binary name: argv array is empty");
            }

            // setup binary name
            serverGlobalParams.binaryName = argv[0];
            size_t i = serverGlobalParams.binaryName.rfind('/');
            if (i != string::npos) {
                serverGlobalParams.binaryName = serverGlobalParams.binaryName.substr(i + 1);
            }
            return Status::OK();
        }

        Status setupCwd() {
                // setup cwd
            char buffer[1024];
#ifdef _WIN32
            verify(_getcwd(buffer, 1000));
#else
            verify(getcwd(buffer, 1000));
#endif
            serverGlobalParams.cwd = buffer;
            return Status::OK();
        }

        Status setArgvArray(const std::vector<std::string>& argv) {
            BSONArrayBuilder b;
            std::vector<std::string> censoredArgv = argv;
            cmdline_utils::censorArgsVector(&censoredArgv);
            for (size_t i=0; i < censoredArgv.size(); i++) {
                b << censoredArgv[i];
            }
            serverGlobalParams.argvArray = b.arr();
            return Status::OK();
        }

        Status setParsedOpts(const moe::Environment& params) {
            serverGlobalParams.parsedOpts = params.toBSON();
            cmdline_utils::censorBSONObj(&serverGlobalParams.parsedOpts);
            return Status::OK();
        }
    } //namespace

    void printCommandLineOpts() {
        log() << "options: " << serverGlobalParams.parsedOpts << endl;
    }

    Status storeServerOptions(const moe::Environment& params,
                              const std::vector<std::string>& args) {

        Status ret = setupBinaryName(args);
        if (!ret.isOK()) {
            return ret;
        }

        ret = setupCwd();
        if (!ret.isOK()) {
            return ret;
        }

        ret = setArgvArray(args);
        if (!ret.isOK()) {
            return ret;
        }

        ret = setParsedOpts(params);
        if (!ret.isOK()) {
            return ret;
        }

        if (params.count("verbose")) {
            std::string verbosity = params["verbose"].as<std::string>();
            for (std::string::iterator iterator = verbosity.begin();
                 iterator != verbosity.end(); iterator++) {
                if (*iterator != 'v') {
                    return Status(ErrorCodes::BadValue,
                                  "The \"verbose\" option string cannot contain any characters "
                                  "other than \"v\"");
                }
            }
        }

        // Handle both the "--verbose" string argument and the "-vvvv" arguments at the same time so
        // that we ensure that we set the log level to the maximum of the options provided
        for (string s = ""; s.length() <= 14; s.append("v")) {
            if (!s.empty() && params.count(s)) {
                logger::globalLogDomain()->setMinimumLoggedSeverity(
                        logger::LogSeverity::Debug(s.length()));
            }

            if (params.count("verbose")) {
                std::string verbosity = params["verbose"].as<std::string>();
                if (s == verbosity) {
                    logger::globalLogDomain()->setMinimumLoggedSeverity(
                            logger::LogSeverity::Debug(s.length()));
                }
            }
        }

        if (params.count("enableExperimentalIndexStatsCmd")) {
            serverGlobalParams.experimental.indexStatsCmdEnabled = true;
        }
        if (params.count("enableExperimentalStorageDetailsCmd")) {
            serverGlobalParams.experimental.storageDetailsCmdEnabled = true;
        }

        if (params.count("port")) {
            serverGlobalParams.port = params["port"].as<int>();
        }

        if (params.count("bind_ip")) {
            serverGlobalParams.bind_ip = params["bind_ip"].as<std::string>();
        }

        if (params.count("clusterAuthMode")) {
            std::string clusterAuthMode = params["clusterAuthMode"].as<std::string>();

            if (clusterAuthMode == "keyFile") {
                serverGlobalParams.clusterAuthMode.store
                    (ServerGlobalParams::ClusterAuthMode_keyFile);
            }
            else if (clusterAuthMode == "sendKeyFile") {
                serverGlobalParams.clusterAuthMode.store
                    (ServerGlobalParams::ClusterAuthMode_sendKeyFile);
            }
            else if (clusterAuthMode == "sendX509") {
                serverGlobalParams.clusterAuthMode.store
                    (ServerGlobalParams::ClusterAuthMode_sendX509);
            }
            else if (clusterAuthMode == "x509") {
                serverGlobalParams.clusterAuthMode.store(ServerGlobalParams::ClusterAuthMode_x509);
            }
            else {
                return Status(ErrorCodes::BadValue, 
                              "unsupported value for clusterAuthMode " + clusterAuthMode );
            }
        }
        else {
            serverGlobalParams.clusterAuthMode.store(ServerGlobalParams::ClusterAuthMode_undefined);
        }
        if (params.count("quiet")) {
            serverGlobalParams.quiet = true;
        }

        if (params.count("traceExceptions")) {
            DBException::traceExceptions = true;
        }

        if (params.count("maxConns")) {
            serverGlobalParams.maxConns = params["maxConns"].as<int>();

            if (serverGlobalParams.maxConns < 5) {
                return Status(ErrorCodes::BadValue, "maxConns has to be at least 5");
            }
        }

        if (params.count("objcheck")) {
            serverGlobalParams.objcheck = true;
        }
        if (params.count("noobjcheck")) {
            if (params.count("objcheck")) {
                return Status(ErrorCodes::BadValue, "can't have both --objcheck and --noobjcheck");
            }
            serverGlobalParams.objcheck = false;
        }

        if (params.count("bind_ip")) {
            // passing in wildcard is the same as default behavior; remove and warn
            if (serverGlobalParams.bind_ip ==  "0.0.0.0") {
                std::cout << "warning: bind_ip of 0.0.0.0 is unnecessary; "
                          << "listens on all ips by default" << endl;
                serverGlobalParams.bind_ip = "";
            }
        }

#ifndef _WIN32
        if (params.count("unixSocketPrefix")) {
            serverGlobalParams.socket = params["unixSocketPrefix"].as<string>();
        }

        if (params.count("nounixsocket")) {
            serverGlobalParams.noUnixSocket = true;
        }

        if (params.count("fork") && !params.count("shutdown")) {
            serverGlobalParams.doFork = true;
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
            serverGlobalParams.logpath = params["logpath"].as<string>();
            if (serverGlobalParams.logpath.empty()) {
                return Status(ErrorCodes::BadValue, "logpath cannot be empty if supplied");
            }
        }

        serverGlobalParams.logWithSyslog = params.count("syslog");

#ifndef _WIN32
        if (params.count("syslogFacility")) {
            std::string facility = params["syslogFacility"].as<string>();
            bool set = false;
            // match facility string to facility value
            for (unsigned long i = 0; i < sizeof(facilitynames)/sizeof(facilitynames[0]); i++) {
                if (facilitynames[i].c_name && !facility.compare(facilitynames[i].c_name)) {
                    serverGlobalParams.syslogFacility = facilitynames[i].c_val;
                    set = true;
                }
            }
            if (!set) {
                StringBuilder sb;
                sb << "ERROR: syslogFacility must be set to a string representing one of the "
                   << "possible syslog facilities";
                return Status(ErrorCodes::BadValue, sb.str());
            }
        }
        else {
            serverGlobalParams.syslogFacility = LOG_USER;
        }
#endif // _WIN32

        serverGlobalParams.logAppend = params.count("logappend");
        if (!serverGlobalParams.logpath.empty() && serverGlobalParams.logWithSyslog) {
            return Status(ErrorCodes::BadValue, "Cant use both a logpath and syslog ");
        }

        if (serverGlobalParams.doFork && serverGlobalParams.logpath.empty() &&
            !serverGlobalParams.logWithSyslog) {
            return Status(ErrorCodes::BadValue, "--fork has to be used with --logpath or --syslog");
        }

        if (params.count("keyFile")) {
            serverGlobalParams.keyFile = boost::filesystem::absolute(
                                            params["keyFile"].as<string>()).generic_string();
        }

        if ( params.count("pidfilepath")) {
            serverGlobalParams.pidFile = params["pidfilepath"].as<string>();
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
            serverGlobalParams.clusterAuthMode.store
                (ServerGlobalParams::ClusterAuthMode_keyFile);
        }

#ifdef MONGO_SSL
        ret = storeSSLServerOptions(params);
        if (!ret.isOK()) {
            return ret;
        }
#endif

        return Status::OK();
    }

    // FIXME: This function will not return the correct value if someone renames the mongos binary
    bool isMongos() { return serverGlobalParams.binaryName == "mongos"; }

} // namespace mongo
