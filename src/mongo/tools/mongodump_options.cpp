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

#include "mongo/tools/mongodump_options.h"

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/util/log.h"
#include "mongo/util/options_parser/environment.h"
#include "mongo/util/options_parser/option_description.h"
#include "mongo/util/options_parser/option_section.h"
#include "mongo/util/options_parser/options_parser.h"

namespace mongo {

    MongoDumpGlobalParams mongoDumpGlobalParams;

    typedef moe::OptionDescription OD;
    typedef moe::PositionalOptionDescription POD;

    Status addMongoDumpOptions(moe::OptionSection* options) {
        Status ret = addGeneralToolOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        ret = addRemoteServerToolOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        ret = addLocalServerToolOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        ret = addSpecifyDBCollectionToolOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        ret = options->addOption(OD("out", "out,o", moe::String,
                    "output directory or \"-\" for stdout", true,
                    moe::Value(std::string("dump"))));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("query", "query,q", moe::String , "json query", true));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("oplog", "oplog", moe::Switch,
                    "Use oplog for point-in-time snapshotting", true));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("repair", "repair", moe::Switch,
                    "try to recover a crashed database", true));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("forceTableScan", "forceTableScan", moe::Switch,
                    "force a table scan (do not use $snapshot)"));
        if(!ret.isOK()) {
            return ret;
        }

        return Status::OK();
    }

    void printMongoDumpHelp(const moe::OptionSection options, std::ostream* out) {
        *out << "Export MongoDB data to BSON files.\n" << std::endl;
        *out << options.helpString();
        *out << std::flush;
    }

    Status handlePreValidationMongoDumpOptions(const moe::Environment& params) {
        if (toolsParsedOptions.count("help")) {
            printMongoDumpHelp(toolsOptions, &std::cout);
            ::_exit(0);
        }
        return Status::OK();
    }

    Status storeMongoDumpOptions(const moe::Environment& params,
                                 const std::vector<std::string>& args) {
        Status ret = storeGeneralToolOptions(params, args);
        if (!ret.isOK()) {
            return ret;
        }

        mongoDumpGlobalParams.repair = hasParam("repair");
        if (mongoDumpGlobalParams.repair){
            if (!hasParam("dbpath")) {
                log() << "repair mode only works with --dbpath" << std::endl;
                ::_exit(-1);
            }

            if (!hasParam("db")) {
                log() << "repair mode only works on 1 db at a time right now" << std::endl;
                ::_exit(-1);
            }
        }
        mongoDumpGlobalParams.query = getParam("query");
        mongoDumpGlobalParams.useOplog = hasParam("oplog");
        if (mongoDumpGlobalParams.useOplog) {
            if (hasParam("query") || hasParam("db") || hasParam("collection")) {
                log() << "oplog mode is only supported on full dumps" << std::endl;
                ::_exit(-1);
            }
        }
        mongoDumpGlobalParams.outputFile = getParam("out");
        mongoDumpGlobalParams.snapShotQuery = false;
        if (!hasParam("query") && !hasParam("dbpath") && !hasParam("forceTableScan")) {
            mongoDumpGlobalParams.snapShotQuery = true;
        }

        // Make the default db "" if it was not explicitly set
        if (!toolsParsedOptions.count("db")) {
            toolGlobalParams.db = "";
        }

        return Status::OK();
    }

    MONGO_INITIALIZER_GENERAL(ParseStartupConfiguration,
            MONGO_NO_PREREQUISITES,
            ("default"))(InitializerContext* context) {

        toolsOptions = moe::OptionSection( "options" );
        moe::OptionsParser parser;

        Status retStatus = addMongoDumpOptions(&toolsOptions);
        if (!retStatus.isOK()) {
            return retStatus;
        }

        retStatus = parser.run(toolsOptions, context->args(), context->env(), &toolsParsedOptions);
        if (!retStatus.isOK()) {
            std::cerr << retStatus.reason() << std::endl;
            std::cerr << "try '" << context->args()[0]
                      << " --help' for more information" << std::endl;
            ::_exit(EXIT_BADOPTIONS);
        }

        retStatus = handlePreValidationMongoDumpOptions(toolsParsedOptions);
        if (!retStatus.isOK()) {
            return retStatus;
        }

        retStatus = toolsParsedOptions.validate();
        if (!retStatus.isOK()) {
            return retStatus;
        }

        retStatus = storeMongoDumpOptions(toolsParsedOptions, context->args());
        if (!retStatus.isOK()) {
            return retStatus;
        }

        return Status::OK();
    }
}
