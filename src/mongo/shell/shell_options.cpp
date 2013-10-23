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
 */

#include "mongo/shell/shell_options.h"

#include <boost/filesystem/operations.hpp>

#include "mongo/base/status.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/server_options.h"
#include "mongo/shell/shell_utils.h"
#include "mongo/util/net/sock.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/options_parser/startup_options.h"
#include "mongo/util/version.h"

namespace mongo {

    ShellGlobalParams shellGlobalParams;

    Status addMongoShellOptions(moe::OptionSection* options) {

        options->addOptionChaining("shell", "shell", moe::Switch,
                "run the shell after executing files");

        options->addOptionChaining("nodb", "nodb", moe::Switch,
                "don't connect to mongod on startup - no 'db address' arg expected");

        options->addOptionChaining("norc", "norc", moe::Switch,
                "will not run the \".mongorc.js\" file on start up");

        options->addOptionChaining("quiet", "quiet", moe::Switch, "be less chatty");

        options->addOptionChaining("port", "port", moe::String, "port to connect to");

        options->addOptionChaining("host", "host", moe::String, "server to connect to");

        options->addOptionChaining("eval", "eval", moe::String, "evaluate javascript");

        options->addOptionChaining("username", "username,u", moe::String,
                "username for authentication");

        options->addOptionChaining("password", "password,p", moe::String,
                "password for authentication")
                                  .setImplicit(moe::Value(std::string("")));

        options->addOptionChaining("authenticationDatabase", "authenticationDatabase", moe::String,
                "user source (defaults to dbname)")
                                  .setDefault(moe::Value(std::string("")));

        options->addOptionChaining("authenticationMechanism", "authenticationMechanism",
                moe::String, "authentication mechanism")
                                  .setDefault(moe::Value(std::string("MONGODB-CR")));

        options->addOptionChaining("help", "help,h", moe::Switch, "show this usage information");

        options->addOptionChaining("version", "version", moe::Switch, "show version information");

        options->addOptionChaining("verbose", "verbose", moe::Switch, "increase verbosity");

        options->addOptionChaining("ipv6", "ipv6", moe::Switch,
                "enable IPv6 support (disabled by default)");

        Status ret = Status::OK();
#ifdef MONGO_SSL
        ret = addSSLClientOptions(options);
        if (!ret.isOK()) {
            return ret;
        }
#endif

        options->addOptionChaining("dbaddress", "dbaddress", moe::String, "dbaddress")
                                  .hidden()
                                  .positional(1, 1);

        options->addOptionChaining("files", "files", moe::StringVector, "files")
                                  .hidden()
                                  .positional(2, -1);

        // for testing, kill op will also be disabled automatically if the tests starts a mongo
        // program
        options->addOptionChaining("nokillop", "nokillop", moe::Switch, "nokillop")
                                  .hidden();

        // for testing, will kill op without prompting
        options->addOptionChaining("autokillop", "autokillop", moe::Switch, "autokillop")
                                  .hidden();


        return Status::OK();
    }

    std::string getMongoShellHelp(const StringData& name, const moe::OptionSection& options) {
        StringBuilder sb;
        sb << "MongoDB shell version: " << mongo::versionString << "\n";
        sb << "usage: " << name << " [options] [db address] [file names (ending in .js)]\n"
        << "db address can be:\n"
        << "  foo                   foo database on local machine\n"
        << "  192.169.0.5/foo       foo database on 192.168.0.5 machine\n"
        << "  192.169.0.5:9999/foo  foo database on 192.168.0.5 machine on port 9999\n"
        << options.helpString() << "\n"
        << "file names: a list of files to run. files have to end in .js and will exit after "
        << "unless --shell is specified";
        return sb.str();
    }

    bool handlePreValidationMongoShellOptions(const moe::Environment& params,
                                                const std::vector<std::string>& args) {
        if (params.count("help")) {
            std::cout << getMongoShellHelp(args[0], moe::startupOptions) << std::endl;
            return true;
        }
        if (params.count("version")) {
            cout << "MongoDB shell version: " << mongo::versionString << endl;
            return true;
        }
        return false;
    }

    Status storeMongoShellOptions(const moe::Environment& params,
                                  const std::vector<std::string>& args) {

        if (params.count("quiet")) {
            mongo::serverGlobalParams.quiet = true;
        }
#ifdef MONGO_SSL
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
            shellGlobalParams.authenticationDatabase =
                params["authenticationDatabase"].as<string>();
        }

        if (params.count("authenticationMechanism")) {
            shellGlobalParams.authenticationMechanism =
                params["authenticationMechanism"].as<string>();
        }

        if (params.count("shell")) {
            shellGlobalParams.runShell = true;
        }
        if (params.count("nodb")) {
            shellGlobalParams.nodb = true;
        }
        if (params.count("norc")) {
            shellGlobalParams.norc = true;
        }
        if (params.count("files")) {
            shellGlobalParams.files = params["files"].as< vector<string> >();
        }
        if (params.count("nokillop")) {
            mongo::shell_utils::_nokillop = true;
        }
        if (params.count("autokillop")) {
            shellGlobalParams.autoKillOp = true;
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
                shellGlobalParams.files.insert( shellGlobalParams.files.begin(), dbaddress );
            }
            else {
                string basename = dbaddress.substr( dbaddress.find_last_of( "/\\" ) + 1 );
                if (basename.find_first_of( '.' ) == string::npos ||
                        (basename.find(".js", basename.size() - 3) == string::npos &&
                         !::mongo::shell_utils::fileExists(dbaddress))) {
                    shellGlobalParams.url = dbaddress;
                }
                else {
                    shellGlobalParams.files.insert( shellGlobalParams.files.begin(), dbaddress );
                }
            }
        }

        if ( shellGlobalParams.url == "*" ) {
            StringBuilder sb;
            sb << "ERROR: " << "\"*\" is an invalid db address";
            sb << getMongoShellHelp(args[0], moe::startupOptions);
            return Status(ErrorCodes::BadValue, sb.str());
        }

        return Status::OK();
    }

}
