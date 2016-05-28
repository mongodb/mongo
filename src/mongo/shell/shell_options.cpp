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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault;

#include "mongo/platform/basic.h"

#include "mongo/shell/shell_options.h"

#include <boost/filesystem/operations.hpp>

#include <iostream>

#include "mongo/base/status.h"
#include "mongo/bson/util/builder.h"
#include "mongo/client/mongo_uri.h"
#include "mongo/client/sasl_client_authenticate.h"
#include "mongo/config.h"
#include "mongo/db/server_options.h"
#include "mongo/rpc/protocol.h"
#include "mongo/shell/shell_utils.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/options_parser/startup_options.h"
#include "mongo/util/version.h"

namespace mongo {

using std::cout;
using std::endl;
using std::string;
using std::vector;

ShellGlobalParams shellGlobalParams;

Status addMongoShellOptions(moe::OptionSection* options) {
    options->addOptionChaining(
        "shell", "shell", moe::Switch, "run the shell after executing files");

    options->addOptionChaining("nodb",
                               "nodb",
                               moe::Switch,
                               "don't connect to mongod on startup - no 'db address' arg expected");

    options->addOptionChaining(
        "norc", "norc", moe::Switch, "will not run the \".mongorc.js\" file on start up");

    options->addOptionChaining("quiet", "quiet", moe::Switch, "be less chatty");

    options->addOptionChaining("port", "port", moe::String, "port to connect to");

    options->addOptionChaining("host", "host", moe::String, "server to connect to");

    options->addOptionChaining("eval", "eval", moe::String, "evaluate javascript");

    moe::OptionSection authenticationOptions("Authentication Options");

    authenticationOptions.addOptionChaining(
        "username", "username,u", moe::String, "username for authentication");

    authenticationOptions
        .addOptionChaining("password", "password,p", moe::String, "password for authentication")
        .setImplicit(moe::Value(std::string("")));

    authenticationOptions
        .addOptionChaining("authenticationDatabase",
                           "authenticationDatabase",
                           moe::String,
                           "user source (defaults to dbname)")
        .setDefault(moe::Value(std::string("")));

    authenticationOptions.addOptionChaining("authenticationMechanism",
                                            "authenticationMechanism",
                                            moe::String,
                                            "authentication mechanism");

    authenticationOptions
        .addOptionChaining("gssapiServiceName",
                           "gssapiServiceName",
                           moe::String,
                           "Service name to use when authenticating using GSSAPI/Kerberos")
        .setDefault(moe::Value(std::string(saslDefaultServiceName)));

    authenticationOptions.addOptionChaining(
        "gssapiHostName",
        "gssapiHostName",
        moe::String,
        "Remote host name to use for purpose of GSSAPI/Kerberos authentication");

    options->addSection(authenticationOptions);

    options->addOptionChaining("help", "help,h", moe::Switch, "show this usage information");

    options->addOptionChaining("version", "version", moe::Switch, "show version information");

    options->addOptionChaining("verbose", "verbose", moe::Switch, "increase verbosity");

    options->addOptionChaining(
        "ipv6", "ipv6", moe::Switch, "enable IPv6 support (disabled by default)");

    options->addOptionChaining("disableJavaScriptJIT",
                               "disableJavaScriptJIT",
                               moe::Switch,
                               "disable the Javascript Just In Time compiler");

    options
        ->addOptionChaining("disableJavaScriptProtection",
                            "disableJavaScriptProtection",
                            moe::Switch,
                            "allow automatic JavaScript function marshalling")
        .incompatibleWith("enableJavaScriptProtection");

    Status ret = Status::OK();
#ifdef MONGO_CONFIG_SSL
    ret = addSSLClientOptions(options);
    if (!ret.isOK()) {
        return ret;
    }
#endif

    options
        ->addOptionChaining("enableJavaScriptProtection",
                            "enableJavaScriptProtection",
                            moe::Switch,
                            "disable automatic JavaScript function marshalling (defaults to true)")
        .hidden()
        .incompatibleWith("disableJavaScriptProtection");

    options->addOptionChaining("dbaddress", "dbaddress", moe::String, "dbaddress")
        .hidden()
        .positional(1, 1);

    options->addOptionChaining("files", "files", moe::StringVector, "files")
        .hidden()
        .positional(2, -1);

    // for testing, kill op will also be disabled automatically if the tests starts a mongo
    // program
    options->addOptionChaining("nokillop", "nokillop", moe::Switch, "nokillop").hidden();

    // for testing, will kill op without prompting
    options->addOptionChaining("autokillop", "autokillop", moe::Switch, "autokillop").hidden();

    options
        ->addOptionChaining("useLegacyWriteOps",
                            "useLegacyWriteOps",
                            moe::Switch,
                            "use legacy write ops instead of write commands")
        .hidden();

    options
        ->addOptionChaining("writeMode",
                            "writeMode",
                            moe::String,
                            "mode to determine how writes are done:"
                            " commands, compatibility, legacy")
        .hidden();

    options
        ->addOptionChaining("readMode",
                            "readMode",
                            moe::String,
                            "mode to determine how .find() queries are done:"
                            " commands, compatibility, legacy")
        .hidden();

    options
        ->addOptionChaining(
            "rpcProtocols", "rpcProtocols", moe::String, " none, opQueryOnly, opCommandOnly, all")
        .hidden();

    return Status::OK();
}

std::string getMongoShellHelp(StringData name, const moe::OptionSection& options) {
    StringBuilder sb;
    sb << "MongoDB shell version: " << mongo::versionString << "\n";
    sb << "usage: " << name << " [options] [db address] [file names (ending in .js)]\n"
       << "db address can be:\n"
       << "  foo                   foo database on local machine\n"
       << "  192.168.0.5/foo       foo database on 192.168.0.5 machine\n"
       << "  192.168.0.5:9999/foo  foo database on 192.168.0.5 machine on port 9999\n"
       << options.helpString() << "\n"
       << "file names: a list of files to run. files have to end in .js and will exit after "
       << "unless --shell is specified";
    return sb.str();
}

bool handlePreValidationMongoShellOptions(const moe::Environment& params,
                                          const std::vector<std::string>& args) {
    if (params.count("help")) {
        std::cout << getMongoShellHelp(args[0], moe::startupOptions) << std::endl;
        return false;
    }
    if (params.count("version")) {
        cout << "MongoDB shell version: " << mongo::versionString << endl;
        return false;
    }
    return true;
}

Status storeMongoShellOptions(const moe::Environment& params,
                              const std::vector<std::string>& args) {
    if (params.count("quiet")) {
        mongo::serverGlobalParams.quiet = true;
    }
#ifdef MONGO_CONFIG_SSL
    Status ret = storeSSLClientOptions(params);
    if (!ret.isOK()) {
        return ret;
    }
#endif
    if (params.count("ipv6")) {
        mongo::enableIPv6();
    }
    if (params.count("verbose")) {
        logger::globalLogDomain()->setMinimumLoggedSeverity(logger::LogSeverity::Debug(1));
    }

    if (params.count("port")) {
        shellGlobalParams.port = params["port"].as<string>();
    }

    if (params.count("host")) {
        shellGlobalParams.dbhost = params["host"].as<string>();
    }

    if (params.count("eval")) {
        shellGlobalParams.script = params["eval"].as<string>();
    }

    if (params.count("username")) {
        shellGlobalParams.username = params["username"].as<string>();
    }

    if (params.count("password")) {
        shellGlobalParams.usingPassword = true;
        shellGlobalParams.password = params["password"].as<string>();
    }

    if (params.count("authenticationDatabase")) {
        shellGlobalParams.authenticationDatabase = params["authenticationDatabase"].as<string>();
    }

    if (params.count("authenticationMechanism")) {
        shellGlobalParams.authenticationMechanism = params["authenticationMechanism"].as<string>();
    }

    if (params.count("gssapiServiceName")) {
        shellGlobalParams.gssapiServiceName = params["gssapiServiceName"].as<string>();
    }

    if (params.count("gssapiHostName")) {
        shellGlobalParams.gssapiHostName = params["gssapiHostName"].as<string>();
    }

    if (params.count("shell")) {
        shellGlobalParams.runShell = true;
    }
    if (params.count("nodb")) {
        shellGlobalParams.nodb = true;
    }
    if (params.count("disableJavaScriptProtection")) {
        shellGlobalParams.javascriptProtection = false;
    }
    if (params.count("norc")) {
        shellGlobalParams.norc = true;
    }
    if (params.count("disableJavaScriptJIT")) {
        shellGlobalParams.nojit = true;
    }
    if (params.count("files")) {
        shellGlobalParams.files = params["files"].as<vector<string>>();
    }
    if (params.count("nokillop")) {
        mongo::shell_utils::_nokillop = true;
    }
    if (params.count("autokillop")) {
        shellGlobalParams.autoKillOp = true;
    }
    if (params.count("useLegacyWriteOps")) {
        shellGlobalParams.writeMode = "legacy";
    }
    if (params.count("writeMode")) {
        std::string mode = params["writeMode"].as<string>();
        if (mode != "commands" && mode != "legacy" && mode != "compatibility") {
            throw MsgAssertionException(
                17396, mongoutils::str::stream() << "Unknown writeMode option: " << mode);
        }
        shellGlobalParams.writeMode = mode;
    }
    if (params.count("readMode")) {
        std::string mode = params["readMode"].as<string>();
        if (mode != "commands" && mode != "compatibility" && mode != "legacy") {
            throw MsgAssertionException(
                17397,
                mongoutils::str::stream()
                    << "Unknown readMode option: '"
                    << mode
                    << "'. Valid modes are: {commands, compatibility, legacy}");
        }
        shellGlobalParams.readMode = mode;
    }
    if (params.count("rpcProtocols")) {
        std::string protos = params["rpcProtocols"].as<string>();
        auto parsedRPCProtos = rpc::parseProtocolSet(protos);
        if (!parsedRPCProtos.isOK()) {
            throw MsgAssertionException(28653,
                                        str::stream() << "Unknown RPC Protocols: '" << protos
                                                      << "'. Valid values are {none, opQueryOnly, "
                                                      << "opCommandOnly, all}");
        }
        shellGlobalParams.rpcProtocols = parsedRPCProtos.getValue();
    }

    /* This is a bit confusing, here are the rules:
     *
     * if nodb is set then all positional parameters are files
     * otherwise the first positional parameter might be a dbaddress, but
     * only if one of these conditions is met:
     *   - it contains no '.' after the last appearance of '\' or '/'
     *   - it doesn't end in '.js' and it doesn't specify a path to an existing file */
    if (params.count("dbaddress")) {
        string dbaddress = params["dbaddress"].as<string>();
        if (shellGlobalParams.nodb) {
            shellGlobalParams.files.insert(shellGlobalParams.files.begin(), dbaddress);
        } else {
            string basename = dbaddress.substr(dbaddress.find_last_of("/\\") + 1);
            if (basename.find_first_of('.') == string::npos ||
                (basename.find(".js", basename.size() - 3) == string::npos &&
                 !::mongo::shell_utils::fileExists(dbaddress))) {
                shellGlobalParams.url = dbaddress;
            } else {
                shellGlobalParams.files.insert(shellGlobalParams.files.begin(), dbaddress);
            }
        }
    }

    if (shellGlobalParams.url == "*") {
        StringBuilder sb;
        sb << "ERROR: "
           << "\"*\" is an invalid db address";
        sb << getMongoShellHelp(args[0], moe::startupOptions);
        return Status(ErrorCodes::BadValue, sb.str());
    }

    if (shellGlobalParams.url.find("mongodb://") == 0) {
        auto cs_status = MongoURI::parse(shellGlobalParams.url);
        if (!cs_status.isOK()) {
            return cs_status.getStatus();
        }

        auto cs = cs_status.getValue();
        StringBuilder sb;
        sb << "ERROR: Cannot specify ";
        auto uriOptions = cs.getOptions();
        if (!shellGlobalParams.username.empty() && !cs.getUser().empty()) {
            sb << "username";
        } else if (!shellGlobalParams.password.empty() && !cs.getPassword().empty()) {
            sb << "password";
        } else if (!shellGlobalParams.authenticationMechanism.empty() &&
                   uriOptions.count("authMechanism")) {
            sb << "the authentication mechanism";
        } else if (!shellGlobalParams.authenticationDatabase.empty() &&
                   uriOptions.count("authSource")) {
            sb << "the authentication database";
        } else if (shellGlobalParams.gssapiServiceName != saslDefaultServiceName &&
                   uriOptions.count("gssapiServiceName")) {
            sb << "the GSSAPI service name";
        } else {
            return Status::OK();
        }
        sb << " in connection URI and as a command-line option";
        return Status(ErrorCodes::InvalidOptions, sb.str());
    }

    return Status::OK();
}
}
