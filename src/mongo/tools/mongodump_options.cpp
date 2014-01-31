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

        options->addOptionChaining("dumpDbUsersAndRoles", "dumpDbUsersAndRoles", moe::Switch,
                "Dump user and role definitions for the given database")
                        .requires("db").incompatibleWith("collection");

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

        // Make the default db "" if it was not explicitly set
        if (!params.count("db")) {
            toolGlobalParams.db = "";
        }

        if (hasParam("dumpDbUsersAndRoles") && toolGlobalParams.db == "admin") {
            return Status(ErrorCodes::BadValue,
                          "Cannot provide --dumpDbUsersAndRoles when dumping the admin db as "
                          "user and role definitions for the whole server are dumped by default "
                          "when dumping the admin db");
        }

        // Always dump users and roles if doing a full dump.  If doing a db dump, only dump users
        // and roles if --dumpDbUsersAndRoles provided or you're dumping the admin db.
        mongoDumpGlobalParams.dumpUsersAndRoles = hasParam("dumpDbUsersAndRoles") ||
                (toolGlobalParams.db.empty() && toolGlobalParams.coll.empty()) ||
                (toolGlobalParams.db == "admin" && toolGlobalParams.coll.empty());

        if (mongoDumpGlobalParams.outputDirectory == "-") {
            // write output to standard error to avoid mangling output
            // must happen early to avoid sending junk to stdout
            toolGlobalParams.canUseStdout = false;
        }

        return Status::OK();
    }

}
