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

#include "mongo/tools/mongoexport_options.h"

#include "mongo/base/status.h"
#include "mongo/util/options_parser/startup_option_init.h"
#include "mongo/util/options_parser/startup_options.h"

namespace mongo {

    MongoExportGlobalParams mongoExportGlobalParams;

    typedef moe::OptionDescription OD;
    typedef moe::PositionalOptionDescription POD;

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

        ret = options->addOption(OD("query", "query,q", moe::String ,
                    "query filter, as a JSON string", true));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("csv", "csv", moe::Switch,
                    "export to csv instead of json", true));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("out", "out,o", moe::String,
                    "output file; if not specified, stdout is used", true));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("jsonArray", "jsonArray", moe::Switch,
                    "output to a json array rather than one object per line", true));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("slaveOk", "slaveOk,k", moe::Bool ,
                    "use secondaries for export if available, default true", true,
                    moe::Value(true)));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("forceTableScan", "forceTableScan", moe::Switch,
                    "force a table scan (do not use $snapshot)", true));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("skip", "skip", moe::Int,
                    "documents to skip, default 0", true, moe::Value(0)));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("limit", "limit", moe::Int,
                    "limit the numbers of documents returned, default all", true,
                    moe::Value(0)));
        if(!ret.isOK()) {
            return ret;
        }

        return Status::OK();
    }

    void printMongoExportHelp(std::ostream* out) {
        *out << "Export MongoDB data to CSV, TSV or JSON files.\n" << std::endl;
        *out << moe::startupOptions.helpString();
        *out << std::flush;
    }

    Status handlePreValidationMongoExportOptions(const moe::Environment& params) {
        if (params.count("help")) {
            printMongoExportHelp(&std::cout);
            ::_exit(0);
        }
        return Status::OK();
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

        return Status::OK();
    }

    MONGO_GENERAL_STARTUP_OPTIONS_REGISTER(MongoExportOptions)(InitializerContext* context) {
        return addMongoExportOptions(&moe::startupOptions);
    }

    MONGO_STARTUP_OPTIONS_VALIDATE(MongoExportOptions)(InitializerContext* context) {
        Status ret = handlePreValidationMongoExportOptions(moe::startupOptionsParsed);
        if (!ret.isOK()) {
            return ret;
        }
        ret = moe::startupOptionsParsed.validate();
        if (!ret.isOK()) {
            return ret;
        }
        return Status::OK();
    }

    MONGO_STARTUP_OPTIONS_STORE(MongoExportOptions)(InitializerContext* context) {
        return storeMongoExportOptions(moe::startupOptionsParsed, context->args());
    }
}
