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

#include "mongo/s/mongos_options.h"

#include <string>
#include <vector>

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/server_options.h"
#include "mongo/s/chunk.h"
#include "mongo/s/version_mongos.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/options_parser/option_description.h"
#include "mongo/util/options_parser/option_section.h"
#include "mongo/util/options_parser/options_parser.h"
#include "mongo/util/startup_test.h"
#include "mongo/util/stringutils.h"

namespace mongo {

    typedef moe::OptionDescription OD;
    typedef moe::PositionalOptionDescription POD;

    MongosGlobalParams mongosGlobalParams;

    Status addMongosOptions(moe::OptionSection* options) {

        moe::OptionSection general_options("General options");

        Status ret = addGeneralServerOptions(&general_options);
        if (!ret.isOK()) {
            return ret;
        }

#if defined(_WIN32)
        moe::OptionSection windows_scm_options("Windows Service Control Manager options");

        ret = addWindowsServerOptions(&windows_scm_options);
        if (!ret.isOK()) {
            return ret;
        }
#endif

#ifdef MONGO_SSL
        moe::OptionSection ssl_options("SSL options");

        ret = addSSLServerOptions(&ssl_options);
        if (!ret.isOK()) {
            return ret;
        }
#endif

        moe::OptionSection sharding_options("Sharding options");

        ret = sharding_options.addOption(OD("configdb", "configdb", moe::String,
                    "1 or 3 comma separated config servers" , true));
        if (!ret.isOK()) {
            return ret;
        }
        ret = sharding_options.addOption(OD("localThreshold", "localThreshold", moe::Int,
                    "ping time (in ms) for a node to be considered local (default 15ms)" , true));
        if (!ret.isOK()) {
            return ret;
        }
        ret = sharding_options.addOption(OD("test", "test", moe::Switch, "just run unit tests",
                    true));
        if (!ret.isOK()) {
            return ret;
        }
        ret = sharding_options.addOption(OD("upgrade", "upgrade", moe::Switch,
                    "upgrade meta data version", true));
        if (!ret.isOK()) {
            return ret;
        }
        ret = sharding_options.addOption(OD("chunkSize", "chunkSize", moe::Int,
                    "maximum amount of data per chunk" , true));
        if (!ret.isOK()) {
            return ret;
        }
        ret = sharding_options.addOption(OD("ipv6", "ipv6", moe::Switch,
                    "enable IPv6 support (disabled by default)", true));
        if (!ret.isOK()) {
            return ret;
        }
        ret = sharding_options.addOption(OD("jsonp", "jsonp", moe::Switch,
                    "allow JSONP access via http (has security implications)", true));
        if (!ret.isOK()) {
            return ret;
        }
        ret = sharding_options.addOption(OD("noscripting", "noscripting", moe::Switch,
                    "disable scripting engine", true));
        if (!ret.isOK()) {
            return ret;
        }

        options->addSection(general_options);

#if defined(_WIN32)
        options->addSection(windows_scm_options);
#endif

        options->addSection(sharding_options);

#ifdef MONGO_SSL
        options->addSection(ssl_options);
#endif

        ret = options->addOption(OD("noAutoSplit", "noAutoSplit", moe::Switch,
                    "do not send split commands with writes", false));
        if (!ret.isOK()) {
            return ret;
        }

        return Status::OK();
    }

    void printMongosHelp(const moe::OptionSection& options) {
        std::cout << options.helpString() << std::endl;
    };

    Status handlePreValidationMongosOptions(const moe::Environment& params,
                                            const std::vector<std::string>& args) {
        if (params.count("help")) {
            printMongosHelp(serverOptions);
            ::_exit(EXIT_SUCCESS);
        }
        if (params.count("version")) {
            printShardingVersionInfo(true);
            ::_exit(EXIT_SUCCESS);
        }

        return Status::OK();
    }

    Status storeMongosOptions(const moe::Environment& params,
                              const std::vector<std::string>& args) {

        Status ret = storeServerOptions(params, args);
        if (!ret.isOK()) {
            std::cerr << "Error storing command line: " << ret.toString() << std::endl;
            ::_exit(EXIT_BADOPTIONS);
        }

        if ( params.count( "chunkSize" ) ) {
            int csize = params["chunkSize"].as<int>();

            // validate chunksize before proceeding
            if ( csize == 0 ) {
                std::cerr << "error: need a non-zero chunksize" << std::endl;
                ::_exit(EXIT_BADOPTIONS);
            }

            if ( !Chunk::setMaxChunkSizeSizeMB( csize ) ) {
                std::cerr << "MaxChunkSize invalid" << std::endl;
                ::_exit(EXIT_BADOPTIONS);
            }
        }

        if (params.count( "port" ) ) {
            int port = params["port"].as<int>();
            if ( port <= 0 || port > 65535 ) {
                out() << "error: port number must be between 1 and 65535" << endl;
                ::_exit(EXIT_FAILURE);
            }
        }

        if ( params.count( "localThreshold" ) ) {
            serverGlobalParams.defaultLocalThresholdMillis = params["localThreshold"].as<int>();
        }

        if ( params.count( "ipv6" ) ) {
            enableIPv6();
        }

        if ( params.count( "jsonp" ) ) {
            serverGlobalParams.jsonp = true;
        }

        if ( params.count( "test" ) ) {
            ::mongo::logger::globalLogDomain()->setMinimumLoggedSeverity(
                    ::mongo::logger::LogSeverity::Debug(5));
            StartupTest::runTests();
            ::_exit(EXIT_SUCCESS);
        }

        if (params.count("noscripting")) {
            // This option currently has no effect for mongos
        }

        if (params.count("httpinterface")) {
            if (params.count("nohttpinterface")) {
                std::cerr << "can't have both --httpinterface and --nohttpinterface" << std::endl;
                ::_exit(EXIT_BADOPTIONS);
            }
            serverGlobalParams.isHttpInterfaceEnabled = true;
        }

        if (params.count("noAutoSplit")) {
            warning() << "running with auto-splitting disabled" << endl;
            Chunk::ShouldAutoSplit = false;
        }

        if ( ! params.count( "configdb" ) ) {
            std::cerr << "error: no args for --configdb" << std::endl;
            ::_exit(EXIT_BADOPTIONS);
        }

        splitStringDelim(params["configdb"].as<std::string>(), &mongosGlobalParams.configdbs, ',');
        if (mongosGlobalParams.configdbs.size() != 1 && mongosGlobalParams.configdbs.size() != 3) {
            std::cerr << "need either 1 or 3 configdbs" << std::endl;
            ::_exit(EXIT_BADOPTIONS);
        }

        if (mongosGlobalParams.configdbs.size() == 1) {
            warning() << "running with 1 config server should be done only for testing purposes "
                      << "and is not recommended for production" << endl;
        }

        mongosGlobalParams.upgrade = params.count("upgrade");

        return Status::OK();
    }

    MONGO_INITIALIZER_GENERAL(ParseStartupConfiguration,
            ("GlobalLogManager"),
            ("default", "completedStartupConfig"))(InitializerContext* context) {

        serverOptions = moe::OptionSection("Allowed options");
        Status ret = addMongosOptions(&serverOptions);
        if (!ret.isOK()) {
            return ret;
        }

        moe::OptionsParser parser;
        ret = parser.run(serverOptions, context->args(), context->env(), &serverParsedOptions);
        if (!ret.isOK()) {
            std::cerr << "Error parsing command line: " << ret.toString() << std::endl;
            std::cerr << "use --help for help" << std::endl;
            ::_exit(EXIT_BADOPTIONS);
        }

        ret = handlePreValidationMongosOptions(serverParsedOptions, context->args());
        if (!ret.isOK()) {
            return ret;
        }

        ret = serverParsedOptions.validate();
        if (!ret.isOK()) {
            return ret;
        }

        ret = storeMongosOptions(serverParsedOptions, context->args());
        if (!ret.isOK()) {
            return ret;
        }

        return Status::OK();
    }

} // namespace mongo
