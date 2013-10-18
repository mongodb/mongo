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

#include "mongo/tools/bsondump_options.h"

#include "mongo/base/status.h"
#include "mongo/util/options_parser/startup_option_init.h"
#include "mongo/util/options_parser/startup_options.h"

namespace mongo {

    BSONDumpGlobalParams bsonDumpGlobalParams;

    typedef moe::OptionDescription OD;
    typedef moe::PositionalOptionDescription POD;

    Status addBSONDumpOptions(moe::OptionSection* options) {
        Status ret = addGeneralToolOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        ret = addBSONToolOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        ret = options->addOption(OD("type", "type", moe::String ,
                    "type of output: json,debug", true, moe::Value(std::string("json"))));
        if(!ret.isOK()) {
            return ret;
        }

        ret = options->addPositionalOption(POD( "file", moe::String, 1 ));
        if(!ret.isOK()) {
            return ret;
        }

        return Status::OK();
    }

    void printBSONDumpHelp(std::ostream* out) {
        *out << "Display BSON objects in a data file.\n" << std::endl;
        *out << "usage: bsondump [options] <bson filename>" << std::endl;
        *out << moe::startupOptions.helpString();
        *out << std::flush;
    }

    bool handlePreValidationBSONDumpOptions(const moe::Environment& params) {
        if (params.count("help")) {
            printBSONDumpHelp(&std::cout);
            return true;
        }
        return false;
    }

    Status storeBSONDumpOptions(const moe::Environment& params,
                                const std::vector<std::string>& args) {
        Status ret = storeGeneralToolOptions(params, args);
        if (!ret.isOK()) {
            return ret;
        }

        ret = storeBSONToolOptions(params, args);
        if (!ret.isOK()) {
            return ret;
        }

        // BSONDump never has a db connection
        toolGlobalParams.noconnection = true;

        bsonDumpGlobalParams.type = getParam("type");
        bsonDumpGlobalParams.file = getParam("file");

        // Make the default db "" if it was not explicitly set
        if (!params.count("db")) {
            toolGlobalParams.db = "";
        }

        // bsondump always outputs data to stdout, so we can't send messages there
        toolGlobalParams.canUseStdout = false;

        return Status::OK();
    }

    MONGO_GENERAL_STARTUP_OPTIONS_REGISTER(BSONDumpOptions)(InitializerContext* context) {
        return addBSONDumpOptions(&moe::startupOptions);
    }

    MONGO_STARTUP_OPTIONS_VALIDATE(BSONDumpOptions)(InitializerContext* context) {
        if (handlePreValidationBSONDumpOptions(moe::startupOptionsParsed)) {
            ::_exit(EXIT_SUCCESS);
        }
        Status ret = moe::startupOptionsParsed.validate();
        if (!ret.isOK()) {
            return ret;
        }
        return Status::OK();
    }

    MONGO_STARTUP_OPTIONS_STORE(BSONDumpOptions)(InitializerContext* context) {
        Status ret = storeBSONDumpOptions(moe::startupOptionsParsed, context->args());
        if (!ret.isOK()) {
            std::cerr << ret.toString() << std::endl;
            std::cerr << "try '" << context->args()[0] << " --help' for more information"
                      << std::endl;
            ::_exit(EXIT_BADOPTIONS);
        }
        return Status::OK();
    }
}
