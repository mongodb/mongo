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

#include "mongo/base/status.h"
#include "mongo/util/log.h"
#include "mongo/util/options_parser/startup_options.h"

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

    void printMongoDumpHelp(std::ostream* out) {
        *out << "Export MongoDB data to BSON files.\n" << std::endl;
        *out << moe::startupOptions.helpString();
        *out << std::flush;
    }

    bool handlePreValidationMongoDumpOptions(const moe::Environment& params) {
        if (params.count("help")) {
            printMongoDumpHelp(&std::cout);
            return true;;
        }
        return false;
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
                return Status(ErrorCodes::BadValue, "repair mode only works with --dbpath");
            }

            if (!hasParam("db")) {
                return Status(ErrorCodes::BadValue,
                              "repair mode only works on 1 db at a time right now");
            }
        }
        mongoDumpGlobalParams.query = getParam("query");
        mongoDumpGlobalParams.useOplog = hasParam("oplog");
        if (mongoDumpGlobalParams.useOplog) {
            if (hasParam("query") || hasParam("db") || hasParam("collection")) {
                return Status(ErrorCodes::BadValue, "oplog mode is only supported on full dumps");
            }
        }
        mongoDumpGlobalParams.outputFile = getParam("out");
        mongoDumpGlobalParams.snapShotQuery = false;
        if (!hasParam("query") && !hasParam("dbpath") && !hasParam("forceTableScan")) {
            mongoDumpGlobalParams.snapShotQuery = true;
        }

        // Make the default db "" if it was not explicitly set
        if (!params.count("db")) {
            toolGlobalParams.db = "";
        }

        if (mongoDumpGlobalParams.outputFile == "-") {
            // write output to standard error to avoid mangling output
            // must happen early to avoid sending junk to stdout
            toolGlobalParams.canUseStdout = false;
        }

        return Status::OK();
    }

}
