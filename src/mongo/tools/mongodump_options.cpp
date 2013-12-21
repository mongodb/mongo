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

#include "mongo/tools/mongodump_options.h"

#include "mongo/base/status.h"
#include "mongo/util/log.h"
#include "mongo/util/options_parser/startup_options.h"

namespace mongo {

    MongoDumpGlobalParams mongoDumpGlobalParams;

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

        options->addOptionChaining("out", "out,o", moe::String,
                "output directory or \"-\" for stdout")
                                  .setDefault(moe::Value(std::string("dump")));

        options->addOptionChaining("query", "query,q", moe::String, "json query");

        options->addOptionChaining("oplog", "oplog", moe::Switch,
                "Use oplog for point-in-time snapshotting");

        options->addOptionChaining("repair", "repair", moe::Switch,
                "try to recover a crashed database");

        options->addOptionChaining("forceTableScan", "forceTableScan", moe::Switch,
                "force a table scan (do not use $snapshot)");

        options->addOptionChaining("listExtents", "listExtents", moe::Switch,
                "list extents for given db collection").requires("dbpath")
                                  .requires("collection").requires("db").hidden();

        options->addOptionChaining("dumpExtent", "dumpExtent", moe::Switch,
                "dump one extent specified by --diskLoc fn:offset")
                                  .requires("diskLoc").requires("dbpath")
                                  .requires("collection").requires("db").hidden();

        options->addOptionChaining("diskLoc", "diskLoc", moe::String,
                "extent diskLoc formatted as: 'fn:offset'")
                                  .format("[0-9]+:[0-9a-fA-F]+", "[file]:[hex offset]")
                                  .requires("dbpath").requires("dumpExtent").hidden();

        return Status::OK();
    }

    void printMongoDumpHelp(std::ostream* out) {
        *out << "Export MongoDB data to BSON files.\n" << std::endl;
        *out << moe::startupOptions.helpString();
        *out << std::flush;
    }

    bool handlePreValidationMongoDumpOptions(const moe::Environment& params) {
        if (!handlePreValidationGeneralToolOptions(params)) {
            return false;
        }
        if (params.count("help")) {
            printMongoDumpHelp(&std::cout);
            return false;;
        }
        return true;
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
        mongoDumpGlobalParams.outputDirectory = getParam("out");
        mongoDumpGlobalParams.snapShotQuery = false;
        if (!hasParam("query") && !hasParam("dbpath") && !hasParam("forceTableScan")) {
            mongoDumpGlobalParams.snapShotQuery = true;
        }
        mongoDumpGlobalParams.listExtents = hasParam("listExtents");
        mongoDumpGlobalParams.dumpExtent = hasParam("dumpExtent");
        mongoDumpGlobalParams.diskLoc = getParam("diskLoc");

        // Make the default db "" if it was not explicitly set
        if (!params.count("db")) {
            toolGlobalParams.db = "";
        }

        if (mongoDumpGlobalParams.outputDirectory == "-") {
            // write output to standard error to avoid mangling output
            // must happen early to avoid sending junk to stdout
            toolGlobalParams.canUseStdout = false;
        }

        return Status::OK();
    }

}
