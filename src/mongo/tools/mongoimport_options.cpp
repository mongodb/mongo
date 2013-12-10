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

#include "mongo/tools/mongoimport_options.h"

#include "mongo/base/status.h"
#include "mongo/util/options_parser/startup_options.h"
#include "mongo/util/text.h"

namespace mongo {

    MongoImportGlobalParams mongoImportGlobalParams;

    Status addMongoImportOptions(moe::OptionSection* options) {
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

        options->addOptionChaining("ignoreBlanks", "ignoreBlanks", moe::Switch,
                "if given, empty fields in csv and tsv will be ignored");

        options->addOptionChaining("type", "type", moe::String,
                "type of file to import.  default: json (json,csv,tsv)");

        options->addOptionChaining("file", "file", moe::String,
                "file to import from; if not specified stdin is used")
                                  .positional(1, 1);

        options->addOptionChaining("drop", "drop", moe::Switch, "drop collection first ");

        options->addOptionChaining("headerline", "headerline", moe::Switch,
                "first line in input file is a header (CSV and TSV only)");

        options->addOptionChaining("upsert", "upsert", moe::Switch,
                "insert or update objects that already exist");

        options->addOptionChaining("upsertFields", "upsertFields", moe::String,
                "comma-separated fields for the query part of the upsert. "
                "You should make sure this is indexed");

        options->addOptionChaining("stopOnError", "stopOnError", moe::Switch,
                "stop importing at first error rather than continuing");

        options->addOptionChaining("jsonArray", "jsonArray", moe::Switch,
                "load a json array, not one item per line. Currently limited to 16MB.");


        options->addOptionChaining("noimport", "noimport", moe::Switch,
                "don't actually import. useful for benchmarking parser")
                                  .hidden();


        return Status::OK();
    }

    void printMongoImportHelp(std::ostream* out) {
        *out << "Import CSV, TSV or JSON data into MongoDB.\n" << std::endl;
        *out << "When importing JSON documents, each document must be a separate line of the input file.\n";
        *out << "\nExample:\n";
        *out << "  mongoimport --host myhost --db my_cms --collection docs < mydocfile.json\n" << std::endl;
        *out << moe::startupOptions.helpString();
        *out << std::flush;
    }

    bool handlePreValidationMongoImportOptions(const moe::Environment& params) {
        if (!handlePreValidationGeneralToolOptions(params)) {
            return false;
        }
        if (params.count("help")) {
            printMongoImportHelp(&std::cout);
            return false;
        }
        return true;
    }

    Status storeMongoImportOptions(const moe::Environment& params,
                                   const std::vector<std::string>& args) {
        Status ret = storeGeneralToolOptions(params, args);
        if (!ret.isOK()) {
            return ret;
        }

        ret = storeFieldOptions(params, args);
        if (!ret.isOK()) {
            return ret;
        }

        mongoImportGlobalParams.filename = getParam("file");
        mongoImportGlobalParams.drop = hasParam("drop");
        mongoImportGlobalParams.ignoreBlanks = hasParam("ignoreBlanks");

        if (hasParam("upsert") || hasParam("upsertFields")) {
            mongoImportGlobalParams.upsert = true;

            string uf = getParam("upsertFields");
            if (uf.empty()) {
                mongoImportGlobalParams.upsertFields.push_back("_id");
            }
            else {
                StringSplitter(uf.c_str(), ",").split(mongoImportGlobalParams.upsertFields);
            }
        }
        else {
            mongoImportGlobalParams.upsert = false;
        }

        mongoImportGlobalParams.doimport = !hasParam("noimport");
        mongoImportGlobalParams.type = getParam("type", "json");
        mongoImportGlobalParams.jsonArray = hasParam("jsonArray");
        mongoImportGlobalParams.headerLine = hasParam("headerline");
        mongoImportGlobalParams.stopOnError = hasParam("stopOnError");

        return Status::OK();
    }

}
