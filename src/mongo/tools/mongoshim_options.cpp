// mongoshim_options.cpp

/*
 *    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/tools/mongoshim_options.h"

#include "mongo/base/status.h"
#include "mongo/util/options_parser/startup_options.h"

namespace mongo {

    MongoShimGlobalParams mongoShimGlobalParams;

    Status addMongoShimOptions(moe::OptionSection* options) {
        Status ret = addGeneralToolOptions(options);
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

        ret = addFieldOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        options->addOptionChaining("load", "load", moe::Switch,
                                   "loat data" );

        options->addOptionChaining("drop", "drop", moe::Switch,
                                   "drop collection before import" );

        options->addOptionChaining("upsert", "upsert", moe::Switch,
                                   "upsert instead of insert" );

        options->addOptionChaining("query", "query,q", moe::String,
                "query filter, as a JSON string, e.g., '{x:{$gt:1}}'");

        options->addOptionChaining("slaveOk", "slaveOk,k", moe::Bool,
                "use secondaries for export if available, default true")
                                  .setDefault(moe::Value(true));

        options->addOptionChaining("forceTableScan", "forceTableScan", moe::Switch,
                "force a table scan (do not use $snapshot)");

        options->addOptionChaining("skip", "skip", moe::Int, "documents to skip, default 0")
                                  .setDefault(moe::Value(0));

        options->addOptionChaining("limit", "limit", moe::Int,
                "limit the numbers of documents returned, default all")
                                  .setDefault(moe::Value(0));

        options->addOptionChaining("sort", "sort", moe::String,
                "sort order, as a JSON string, e.g., '{x:1}'");

        options->findOption( "dbpath" )->setDefault( moe::Value(std::string("/data/db")) );

        return Status::OK();
    }

    void printMongoShimHelp(std::ostream* out) {
        *out << "Shim MongoDB data to CSV, TSV or JSON files.\n" << std::endl;
        *out << moe::startupOptions.helpString();
        *out << std::flush;
    }

    bool handlePreValidationMongoShimOptions(const moe::Environment& params) {
        if (!handlePreValidationGeneralToolOptions(params)) {
            return false;
        }
        if (params.count("help")) {
            printMongoShimHelp(&std::cout);
            return false;
        }
        return true;
    }

    Status storeMongoShimOptions(const moe::Environment& params,
                                   const std::vector<std::string>& args) {

        Status ret = storeGeneralToolOptions(params, args);
        if (!ret.isOK()) {
            return ret;
        }

        invariant( toolGlobalParams.useDirectClient );

        ret = storeFieldOptions(params, args);
        if (!ret.isOK()) {
            return ret;
        }

        mongoShimGlobalParams.load = params.count("load") > 0;
        mongoShimGlobalParams.drop = params.count("drop") > 0;
        mongoShimGlobalParams.upsert = params.count("upsert") > 0;

        mongoShimGlobalParams.query = getParam("query", "");
        mongoShimGlobalParams.snapShotQuery = false;

        // Only allow snapshot query (requires _id idx scan) if following conditions are false
        if (!hasParam("query") &&
            !hasParam("sort") &&
            !hasParam("dbpath") &&
            !hasParam("forceTableScan")) {
            mongoShimGlobalParams.snapShotQuery = true;
        }

        mongoShimGlobalParams.slaveOk = params["slaveOk"].as<bool>();
        mongoShimGlobalParams.limit = getParam("limit", 0);
        mongoShimGlobalParams.skip = getParam("skip", 0);
        mongoShimGlobalParams.sort = getParam("sort", "");

        toolGlobalParams.canUseStdout = false;

        return Status::OK();
    }

}
