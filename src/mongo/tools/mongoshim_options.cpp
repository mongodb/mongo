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
#include "mongo/db/json.h"
#include "mongo/util/options_parser/startup_options.h"
#include "mongo/util/text.h"

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

        ret = addBSONToolOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        const string modeDisplayFormat("find, insert, upsert, remove, repair or applyOps");
        options->addOptionChaining("mode", "mode", moe::String,
                "Runs shim in one of several modes: " + modeDisplayFormat)
                                  .format("find|insert|upsert|remove|repair|applyOps",
                                          modeDisplayFormat)
                                  .incompatibleWith("load")
                                  .incompatibleWith("remove")
                                  .incompatibleWith("repair")
                                  .incompatibleWith("applyOps");

        options->addOptionChaining("load", "load", moe::Switch,
                                   "load data" )
                                  .hidden();

        options->addOptionChaining("remove", "remove", moe::Switch,
                "removes documents from collection matching query "
                "(or all documents if query is not provided)" )
                                  .incompatibleWith("load")
                                  .incompatibleWith("drop")
                                  .hidden();

        options->addOptionChaining("applyOps", "applyOps", moe::Switch,
                                   "apply oplog entries" )
                                  .incompatibleWith("load")
                                  .incompatibleWith("drop")
                                  .hidden();

        options->addOptionChaining("drop", "drop", moe::Switch,
                                   "drop collection before import" );

        options->addOptionChaining("upsert", "upsert", moe::Switch,
                                   "upsert instead of insert" )
                                  .requires("load")
                                  .hidden();

        options->addOptionChaining("upsertFields", "upsertFields", moe::String,
                "comma-separated fields for the query part of the upsert. "
                "Ensure these fields are indexed.");

        options->addOptionChaining("query", "query,q", moe::String,
                "query filter, as a JSON string, e.g., '{x:{$gt:1}}'");

        options->addOptionChaining("repair", "repair", moe::Switch,
                "try to recover a crashed collection")
                                  .incompatibleWith("applyOps")
                                  .incompatibleWith("load")
                                  .incompatibleWith("remove")
                                  .hidden();

        // Used for testing.
        options->addOptionChaining("in", "in", moe::String,
                "input file; if not specified, stdin is used")
                                  .hidden();

        // Used for testing.
        options->addOptionChaining("inputDocuments", "inputDocuments", moe::String,
                "input documents. If specified, stdin will be ignored. "
                "Format: --inputDocuments=\"{in: [doc1, doc2, doc3, ...]}\". ")
                                  .incompatibleWith("in")
                                  .hidden();
        // Used for testing.
        options->addOptionChaining("out", "out", moe::String,
                "output file; if not specified, stdout is used")
                                  .incompatibleWith("in")
                                  .hidden();

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

        return Status::OK();
    }

    void printMongoShimHelp(std::ostream* out) {
        *out << "Read/write directly to stored format.\n" << std::endl;
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

        if (!toolGlobalParams.useDirectClient) {
            return Status(ErrorCodes::BadValue,
                          "MongoShim requires a --dbpath value to proceed");
        }

        // Ensure that collection is specified.
        // Tool::getNs() validates --collection but error
        // is not propagated to calling process because Tool::main()
        // always exits cleanly when --dbpath is enabled.
        if (toolGlobalParams.coll.size() == 0) {
            return Status(ErrorCodes::BadValue,
                          "MongoShim requires a --collection value to proceed");
        }

        ret = storeFieldOptions(params, args);
        if (!ret.isOK()) {
            return ret;
        }

        ret = storeBSONToolOptions(params, args);
        if (!ret.isOK()) {
            return ret;
        }

        mongoShimGlobalParams.load = params.count("load") > 0;
        mongoShimGlobalParams.remove = params.count("remove") > 0;
        mongoShimGlobalParams.applyOps = params.count("applyOps") > 0;
        mongoShimGlobalParams.repair = params.count("repair") > 0;

        mongoShimGlobalParams.drop = params.count("drop") > 0;
        mongoShimGlobalParams.upsert = params.count("upsert") > 0;

        // Process shim mode. Using --mode is preferred to --load, --applyOps, --remove, ...
        if (params.count("mode") > 0) {
            const string mode = getParam("mode");
            if (mode == "find") {
                // No change to mongoShimGlobalParams.
            }
            else if (mode == "insert") {
                mongoShimGlobalParams.load = true;
            }
            else if (mode == "remove") {
                mongoShimGlobalParams.remove = true;
            }
            else if (mode == "repair") {
                mongoShimGlobalParams.repair = true;
            }
            else if (mode == "upsert") {
                mongoShimGlobalParams.load = true;
                mongoShimGlobalParams.upsert = true;
            }
            else if (mode == "applyOps") {
                mongoShimGlobalParams.applyOps = true;
            }
        }

        if (mongoShimGlobalParams.upsert) {
            string uf = getParam("upsertFields");
            if (uf.empty()) {
                mongoShimGlobalParams.upsertFields.push_back("_id");
            }
            else {
                StringSplitter(uf.c_str(), ",").split(mongoShimGlobalParams.upsertFields);
            }
        }

        // Used for testing. In normal operation, results will be written to stdout.
        mongoShimGlobalParams.outputFile = getParam("out");
        mongoShimGlobalParams.outputFileSpecified = params.count("out") > 0;

        // Used for testing. In normal operation, documents will be read from stdin.
        mongoShimGlobalParams.inputFile = getParam("in");
        mongoShimGlobalParams.inputFileSpecified = params.count("in") > 0;

        // Used for testing. In normal operation, documents will be read from stdin.
        mongoShimGlobalParams.inputDocuments = mongo::fromjson(getParam("inputDocuments", "{}"));

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
