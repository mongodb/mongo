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

#include "mongo/platform/basic.h"

#include "mongo/tools/mongoshim_options.h"

#include <vector>

#include "mongo/base/status.h"
#include "mongo/db/json.h"
#include "mongo/util/options_parser/startup_options.h"
#include "mongo/util/stringutils.h"
#include "mongo/util/text.h"

using std::string;
using std::vector;

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

        // Required option --mode.
        // Refer to shim_mode.h for list of supported modes.
        vector<string> modeStrings;
        for (int i = 0; i <= int(ShimMode::kNumShimModes); ++i) {
            ShimMode mode = static_cast<ShimMode::Value>(i);
            modeStrings.push_back(mode.toString());
            for (int i = 0; i <= int(ShimMode::kNumShimModes); ++i) {
                ShimMode mode = static_cast<ShimMode::Value>(i);
                modeStrings.push_back(mode.toString());
            }
        }
        string modeDisplayFormat;
        joinStringDelim(modeStrings, &modeDisplayFormat, '|');
        options->addOptionChaining("mode", "mode", moe::String,
                "Runs shim in one of several modes: " + modeDisplayFormat)
                                  .format(modeDisplayFormat,
                                          modeDisplayFormat);

        // Compatible with --mode=insert only.
        // Used to drop collection before inserting documents read from input.
        options->addOptionChaining("drop", "drop", moe::Switch,
                                   "drop collection before inserting documents");

        // Compatible with --mode=upsert only.
        // Used to specify fields for matching existing documents when
        // updating/inserting.
        options->addOptionChaining("upsertFields", "upsertFields", moe::String,
                "comma-separated fields for the query part of the upsert. "
                "Ensure these fields are indexed.");

        // Used to filter document results before writing to to output.
        options->addOptionChaining("query", "query,q", moe::String,
                "query filter, as a JSON string, e.g., '{x:{$gt:1}}'");

        options->addOptionChaining("skip", "skip", moe::Int, "documents to skip, default 0")
                                  .setDefault(moe::Value(0));

        options->addOptionChaining("limit", "limit", moe::Int,
                "limit the numbers of documents returned, default all")
                                  .setDefault(moe::Value(0));

        options->addOptionChaining("sort", "sort", moe::String,
                "sort order, as a JSON string, e.g., '{x:1}'");

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
                                  .hidden();

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

        ret = storeFieldOptions(params, args);
        if (!ret.isOK()) {
            return ret;
        }

        ret = storeBSONToolOptions(params, args);
        if (!ret.isOK()) {
            return ret;
        }

        // Process shim mode.
        if (params.count("mode") == 0) {
            return Status(ErrorCodes::BadValue,
                          "MongoShim requires a --mode value to proceed");
        }

        const string modeParam = getParam("mode");
        for (int i = 0; i <= int(ShimMode::kNumShimModes); ++i) {
            ShimMode mode = static_cast<ShimMode::Value>(i);
            if (modeParam == mode.toString()) {
                mongoShimGlobalParams.mode = mode;
                break;
            }
        }
        // --mode value is enforced by format string in option description.
        invariant(mongoShimGlobalParams.mode != ShimMode::kNumShimModes);

        mongoShimGlobalParams.drop = params.count("drop") > 0;

        // Ensure that collection is specified when mode is not "command".
        // Tool::getNs() validates --collection but error
        // is not propagated to calling process because Tool::main()
        // always exits cleanly when --dbpath is enabled.
        if (toolGlobalParams.coll.size() == 0 &&
            !(mongoShimGlobalParams.mode == ShimMode::kCommand)) {
            return Status(ErrorCodes::BadValue,
                          "MongoShim requires a --collection value to proceed when not running "
                          "in \"command\" mode");
        }
        // --drop and --collection are not allowed in "command" mode.
        if (mongoShimGlobalParams.mode == ShimMode::kCommand) {
            if (!toolGlobalParams.coll.empty()) {
                return Status(ErrorCodes::BadValue,
                              "--collection is not allowed in \"command\" mode");
            }
            if (mongoShimGlobalParams.drop) {
                return Status(ErrorCodes::BadValue,
                              "--drop is not allowed in \"command\" mode");
            }
        }

        // Parse upsert fields.
        // --upsertFields is illegal when not in "upsert" mode.
        if (params.count("upsertFields") > 0) {
            if (mongoShimGlobalParams.mode != ShimMode::kUpsert) {
                return Status(ErrorCodes::BadValue,
                              "--upsertFields is not allowed when not in \"upsert\" mode");
            }
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

        mongoShimGlobalParams.limit = getParam("limit", 0);
        mongoShimGlobalParams.skip = getParam("skip", 0);
        mongoShimGlobalParams.sort = getParam("sort", "");

        toolGlobalParams.canUseStdout = false;

        return Status::OK();
    }

}
