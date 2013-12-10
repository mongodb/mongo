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

#include "mongo/tools/mongoexport_options.h"

#include "mongo/base/status.h"
#include "mongo/util/options_parser/startup_options.h"

namespace mongo {

    MongoExportGlobalParams mongoExportGlobalParams;

    Status addMongoExportOptions(moe::OptionSection* options) {
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

        ret = addFieldOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        options->addOptionChaining("query", "query,q", moe::String,
                "query filter, as a JSON string");

        options->addOptionChaining("csv", "csv", moe::Switch, "export to csv instead of json");

        options->addOptionChaining("out", "out,o", moe::String,
                "output file; if not specified, stdout is used");

        options->addOptionChaining("jsonArray", "jsonArray", moe::Switch,
                "output to a json array rather than one object per line");

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


        return Status::OK();
    }

    void printMongoExportHelp(std::ostream* out) {
        *out << "Export MongoDB data to CSV, TSV or JSON files.\n" << std::endl;
        *out << moe::startupOptions.helpString();
        *out << std::flush;
    }

    bool handlePreValidationMongoExportOptions(const moe::Environment& params) {
        if (!handlePreValidationGeneralToolOptions(params)) {
            return false;
        }
        if (params.count("help")) {
            printMongoExportHelp(&std::cout);
            return false;
        }
        return true;
    }

    Status storeMongoExportOptions(const moe::Environment& params,
                                   const std::vector<std::string>& args) {
        Status ret = storeGeneralToolOptions(params, args);
        if (!ret.isOK()) {
            return ret;
        }

        ret = storeFieldOptions(params, args);
        if (!ret.isOK()) {
            return ret;
        }

        mongoExportGlobalParams.outputFile = getParam("out");
        mongoExportGlobalParams.outputFileSpecified = hasParam("out");
        mongoExportGlobalParams.csv = hasParam("csv");
        mongoExportGlobalParams.jsonArray = hasParam("jsonArray");
        mongoExportGlobalParams.query = getParam("query", "");
        mongoExportGlobalParams.snapShotQuery = false;
        if (!hasParam("query") && !hasParam("dbpath") && !hasParam("forceTableScan")) {
            mongoExportGlobalParams.snapShotQuery = true;
        }
        mongoExportGlobalParams.slaveOk = params["slaveOk"].as<bool>();
        mongoExportGlobalParams.limit = getParam("limit", 0);
        mongoExportGlobalParams.skip = getParam("skip", 0);

        // we write output to standard error by default to avoid mangling output, but we don't need
        // to do this if an output file was specified
        toolGlobalParams.canUseStdout = false;
        if (mongoExportGlobalParams.outputFileSpecified) {
            if (mongoExportGlobalParams.outputFile != "-") {
                toolGlobalParams.canUseStdout = true;
            }
        }

        return Status::OK();
    }

}
