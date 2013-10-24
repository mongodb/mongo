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

#include "mongo/tools/tool_options.h"

#include <boost/filesystem/operations.hpp>
#include <fstream>
#include "pcrecpp.h"

#include "mongo/base/status.h"
#include "mongo/db/storage_options.h"
#include "mongo/util/log.h"
#include "mongo/util/net/sock.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/options_parser/startup_options.h"
#include "mongo/util/password.h"
#include "mongo/util/text.h"
#include "mongo/util/version.h"

namespace mongo {

    ToolGlobalParams toolGlobalParams;
    BSONToolGlobalParams bsonToolGlobalParams;

    Status addGeneralToolOptions(moe::OptionSection* options) {
        options->addOptionChaining("help", "help", moe::Switch, "produce help message");

        options->addOptionChaining("verbose", "verbose,v", moe::Switch,
                "be more verbose (include multiple times "
                "for more verbosity e.g. -vvvvv)");

        options->addOptionChaining("quiet", "quiet", moe::Switch,
                "silence all non error diagnostic messages");

        options->addOptionChaining("version", "version", moe::Switch,
                "print the program's version and exit");


        /* support for -vv -vvvv etc. */
        for (string s = "vv"; s.length() <= 10; s.append("v")) {
            options->addOptionChaining(s.c_str(), s.c_str(), moe::Switch, "verbose")
                                      .hidden();
        }

        return Status::OK();
    }

    Status addRemoteServerToolOptions(moe::OptionSection* options) {
        options->addOptionChaining("host", "host,h", moe::String,
                "mongo host to connect to ( <set name>/s1,s2 for sets)");

        options->addOptionChaining("port", "port", moe::String,
                "server port. Can also use --host hostname:port");

        options->addOptionChaining("ipv6", "ipv6", moe::Switch,
                "enable IPv6 support (disabled by default)");


#ifdef MONGO_SSL
        Status ret = addSSLClientOptions(options);
        if (!ret.isOK()) {
            return ret;
        }
#endif

        options->addOptionChaining("username", "username,u", moe::String,
                "username");

        // We ask a user for a password if they pass in an empty string or pass --password with no
        // argument.  This must be handled when the password value is checked.
        //
        // Desired behavior:
        // --username test // Continue with username "test" and no password
        // --username test --password test // Continue with username "test" and password "test"
        // --username test --password // Continue with username "test" and prompt for password
        // --username test --password "" // Continue with username "test" and prompt for password
        //
        // To do this we pass moe::Value(std::string("")) as the "implicit value" of this option
        options->addOptionChaining("password", "password,p", moe::String, "password")
                                  .setImplicit(moe::Value(std::string("")));

        options->addOptionChaining("authenticationDatabase", "authenticationDatabase", moe::String,
                "user source (defaults to dbname)")
                                  .setDefault(moe::Value(std::string("")));

        options->addOptionChaining("authenticationMechanism", "authenticationMechanism",
                moe::String, "authentication mechanism")
                                  .setDefault(moe::Value(std::string("MONGODB-CR")));


        return Status::OK();
    }

    Status addLocalServerToolOptions(moe::OptionSection* options) {
        options->addOptionChaining("dbpath", "dbpath", moe::String,
                "directly access mongod database files in the given path, instead of "
                "connecting to a mongod  server - needs to lock the data directory, "
                "so cannot be used if a mongod is currently accessing the same path");

        options->addOptionChaining("directoryperdb", "directoryperdb", moe::Switch,
                "each db is in a separate directory (relevant only if dbpath specified)");

        options->addOptionChaining("journal", "journal", moe::Switch,
                "enable journaling (relevant only if dbpath specified)");


        return Status::OK();
    }
    Status addSpecifyDBCollectionToolOptions(moe::OptionSection* options) {
        options->addOptionChaining("db", "db,d", moe::String, "database to use");

        options->addOptionChaining("collection", "collection,c", moe::String,
                "collection to use (some commands)");


        return Status::OK();
    }

    Status addFieldOptions(moe::OptionSection* options) {
        options->addOptionChaining("fields", "fields,f", moe::String,
                "comma separated list of field names e.g. -f name,age");

        options->addOptionChaining("fieldFile", "fieldFile", moe::String,
                "file with field names - 1 per line");


        return Status::OK();
    }

    Status addBSONToolOptions(moe::OptionSection* options) {
        options->addOptionChaining("objcheck", "objcheck", moe::Switch,
                "validate object before inserting (default)");

        options->addOptionChaining("noobjcheck", "noobjcheck", moe::Switch,
                "don't validate object before inserting");

        options->addOptionChaining("filter", "filter", moe::String,
                "filter to apply before inserting");


        return Status::OK();
    }

    std::string getParam(std::string name, std::string def) {
        if (moe::startupOptionsParsed.count(name)) {
            return moe::startupOptionsParsed[name.c_str()].as<string>();
        }
        return def;
    }
    int getParam(std::string name, int def) {
        if (moe::startupOptionsParsed.count(name)) {
            return moe::startupOptionsParsed[name.c_str()].as<int>();
        }
        return def;
    }
    bool hasParam(std::string name) {
        return moe::startupOptionsParsed.count(name);
    }

    void printToolVersionString(std::ostream &out) {
        out << toolGlobalParams.name << " version " << mongo::versionString;
        if (mongo::versionString[strlen(mongo::versionString)-1] == '-')
            out << " (commit " << mongo::gitVersion() << ")";
        out << std::endl;
    }

    bool handlePreValidationGeneralToolOptions(const moe::Environment& params) {
        if (moe::startupOptionsParsed.count("version")) {
            printToolVersionString(std::cout);
            return false;
        }
        return true;
    }

    extern bool directoryperdb;

    Status storeGeneralToolOptions(const moe::Environment& params,
                                   const std::vector<std::string>& args) {

        toolGlobalParams.name = args[0];

        storageGlobalParams.prealloc = false;

        // The default value may vary depending on compile options, but for tools
        // we want durability to be disabled.
        storageGlobalParams.dur = false;

        // Set authentication parameters
        if (params.count("authenticationDatabase")) {
            toolGlobalParams.authenticationDatabase =
                params["authenticationDatabase"].as<string>();
        }

        if (params.count("authenticationMechanism")) {
            toolGlobalParams.authenticationMechanism =
                params["authenticationMechanism"].as<string>();
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

        toolGlobalParams.quiet = params.count("quiet");

#ifdef MONGO_SSL
        Status ret = storeSSLClientOptions(params);
        if (!ret.isOK()) {
            return ret;
        }
#endif

        if (args.empty()) {
            return Status(ErrorCodes::InternalError, "Cannot get binary name: argv array is empty");
        }

        // setup binary name
        toolGlobalParams.name = args[0];
        size_t i = toolGlobalParams.name.rfind('/');
        if (i != string::npos) {
            toolGlobalParams.name = toolGlobalParams.name.substr(i + 1);
        }
        toolGlobalParams.db = "test";
        toolGlobalParams.coll = "";
        toolGlobalParams.noconnection = false;

        if (params.count("db"))
            toolGlobalParams.db = params["db"].as<string>();

        if (params.count("collection"))
            toolGlobalParams.coll = params["collection"].as<string>();

        if (params.count("username"))
            toolGlobalParams.username = params["username"].as<string>();

        if (params.count("password")) {
            toolGlobalParams.password = params["password"].as<string>();
            if (toolGlobalParams.password.empty()) {
                toolGlobalParams.password = askPassword();
            }
        }

        if (params.count("ipv6")) {
            enableIPv6();
        }

        toolGlobalParams.dbpath = getParam("dbpath");
        toolGlobalParams.useDirectClient = hasParam("dbpath");
        if (toolGlobalParams.useDirectClient && params.count("journal")) {
            storageGlobalParams.dur = true;
        }

        if (!toolGlobalParams.useDirectClient) {
            toolGlobalParams.connectionString = "127.0.0.1";
            if (params.count("host")) {
                toolGlobalParams.hostSet = true;
                toolGlobalParams.host = params["host"].as<string>();
                toolGlobalParams.connectionString = params["host"].as<string>();
            }

            if (params.count("port")) {
                toolGlobalParams.portSet = true;
                toolGlobalParams.port = params["port"].as<string>();
                toolGlobalParams.connectionString += ':' + params["port"].as<string>();
            }
        }
        else {
            if (params.count("directoryperdb")) {
                storageGlobalParams.directoryperdb = true;
            }

            toolGlobalParams.connectionString = "DIRECT";
        }

        return Status::OK();
    }

    Status storeFieldOptions(const moe::Environment& params,
                             const std::vector<std::string>& args) {

        toolGlobalParams.fieldsSpecified = false;

        if (hasParam("fields")) {
            toolGlobalParams.fieldsSpecified = true;

            string fields_arg = getParam("fields");
            pcrecpp::StringPiece input(fields_arg);

            string f;
            pcrecpp::RE re("([#\\w\\.\\s\\-]+),?" );
            while ( re.Consume( &input, &f ) ) {
                toolGlobalParams.fields.push_back( f );
            }
            return Status::OK();
        }

        if (hasParam("fieldFile")) {
            toolGlobalParams.fieldsSpecified = true;

            string fn = getParam("fieldFile");
            if (!boost::filesystem::exists(fn)) {
                StringBuilder sb;
                sb << "file: " << fn << " doesn't exist";
                return Status(ErrorCodes::InternalError, sb.str());
            }

            const int BUF_SIZE = 1024;
            char line[1024 + 128];
            std::ifstream file(fn.c_str());

            while (file.rdstate() == std::ios_base::goodbit) {
                file.getline(line, BUF_SIZE);
                const char * cur = line;
                while (isspace(cur[0])) cur++;
                if (cur[0] == '\0')
                    continue;

                toolGlobalParams.fields.push_back(cur);
            }
            return Status::OK();
        }

        return Status::OK();
    }


    Status storeBSONToolOptions(const moe::Environment& params,
                                const std::vector<std::string>& args) {

        bsonToolGlobalParams.objcheck = true;

        if (hasParam("objcheck") && hasParam("noobjcheck")) {
            return Status(ErrorCodes::BadValue, "can't have both --objcheck and --noobjcheck");
        }

        if (hasParam("objcheck")) {
            bsonToolGlobalParams.objcheck = true;
        }
        else if (hasParam("noobjcheck")) {
            bsonToolGlobalParams.objcheck = false;
        }

        if (hasParam("filter")) {
            bsonToolGlobalParams.hasFilter = true;
            bsonToolGlobalParams.filter = getParam("filter");
        }

        return Status::OK();
    }
}
