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

#include "mongo/tools/mongoimport_options.h"

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/util/options_parser/environment.h"
#include "mongo/util/options_parser/option_description.h"
#include "mongo/util/options_parser/option_section.h"
#include "mongo/util/options_parser/options_parser.h"
#include "mongo/util/text.h"

namespace mongo {

    MongoImportGlobalParams mongoImportGlobalParams;

    typedef moe::OptionDescription OD;
    typedef moe::PositionalOptionDescription POD;

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

        ret = options->addOption(OD("ignoreBlanks", "ignoreBlanks", moe::Switch,
                    "if given, empty fields in csv and tsv will be ignored", true));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("type", "type",moe::String,
                    "type of file to import.  default: json (json,csv,tsv)", true));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("file", "file",moe::String,
                    "file to import from; if not specified stdin is used", true));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("drop", "drop", moe::Switch, "drop collection first ", true));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("headerline", "headerline", moe::Switch,
                    "first line in input file is a header (CSV and TSV only)", true));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("upsert", "upsert", moe::Switch,
                    "insert or update objects that already exist", true));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("upsertFields", "upsertFields", moe::String,
                    "comma-separated fields for the query part of the upsert. "
                    "You should make sure this is indexed", true));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("stopOnError", "stopOnError", moe::Switch,
                    "stop importing at first error rather than continuing", true));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("jsonArray", "jsonArray", moe::Switch,
                    "load a json array, not one item per line. "
                    "Currently limited to 16MB.", true));
        if(!ret.isOK()) {
            return ret;
        }

        ret = options->addOption(OD("noimport", "noimport", moe::Switch,
                    "don't actually import. useful for benchmarking parser", false));
        if(!ret.isOK()) {
            return ret;
        }

        ret = options->addPositionalOption(POD( "file", moe::String, 1 ));
        if(!ret.isOK()) {
            return ret;
        }

        return Status::OK();
    }

    void printMongoImportHelp(const moe::OptionSection options, std::ostream* out) {
        *out << "Import CSV, TSV or JSON data into MongoDB.\n" << std::endl;
        *out << "When importing JSON documents, each document must be a separate line of the input file.\n";
        *out << "\nExample:\n";
        *out << "  mongoimport --host myhost --db my_cms --collection docs < mydocfile.json\n" << std::endl;
        *out << options.helpString();
        *out << std::flush;
    }

    Status handlePreValidationMongoImportOptions(const moe::Environment& params) {
        if (toolsParsedOptions.count("help")) {
            printMongoImportHelp(toolsOptions, &std::cout);
            ::_exit(0);
        }
        return Status::OK();
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

    MONGO_INITIALIZER_GENERAL(ParseStartupConfiguration,
            MONGO_NO_PREREQUISITES,
            ("default"))(InitializerContext* context) {

        toolsOptions = moe::OptionSection( "options" );
        moe::OptionsParser parser;

        Status retStatus = addMongoImportOptions(&toolsOptions);
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

        retStatus = handlePreValidationMongoImportOptions(toolsParsedOptions);
        if (!retStatus.isOK()) {
            return retStatus;
        }

        retStatus = toolsParsedOptions.validate();
        if (!retStatus.isOK()) {
            return retStatus;
        }

        retStatus = storeMongoImportOptions(toolsParsedOptions, context->args());
        if (!retStatus.isOK()) {
            return retStatus;
        }

        return Status::OK();
    }
}
