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
#include "mongo/util/options_parser/environment.h"
#include "mongo/util/options_parser/option_description.h"
#include "mongo/util/options_parser/option_section.h"
#include "mongo/util/options_parser/options_parser.h"
#include "mongo/util/options_parser/startup_option_init.h"

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

    void printBSONDumpHelp(const moe::OptionSection options, std::ostream* out) {
        *out << "Display BSON objects in a data file.\n" << std::endl;
        *out << "usage: bsondump [options] <bson filename>" << std::endl;
        *out << options.helpString();
        *out << std::flush;
    }

    Status handlePreValidationBSONDumpOptions(const moe::Environment& params) {
        if (toolsParsedOptions.count("help")) {
            printBSONDumpHelp(toolsOptions, &std::cout);
            ::_exit(0);
        }
        return Status::OK();
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
        if (!toolsParsedOptions.count("db")) {
            toolGlobalParams.db = "";
        }

        return Status::OK();
    }

    MONGO_GENERAL_STARTUP_OPTIONS_REGISTER(BSONDumpOptions)(InitializerContext* context) {
        return addBSONDumpOptions(&toolsOptions);
    }

    MONGO_STARTUP_OPTIONS_PARSE(BSONDumpOptions)(InitializerContext* context) {
        moe::OptionsParser parser;
        Status ret = parser.run(toolsOptions, context->args(), context->env(),
                                &toolsParsedOptions);
        if (!ret.isOK()) {
            std::cerr << ret.reason() << std::endl;
            std::cerr << "try '" << context->args()[0]
                      << " --help' for more information" << std::endl;
            ::_exit(EXIT_BADOPTIONS);
        }
        return Status::OK();
    }

    MONGO_STARTUP_OPTIONS_VALIDATE(BSONDumpOptions)(InitializerContext* context) {
        Status ret = handlePreValidationBSONDumpOptions(toolsParsedOptions);
        if (!ret.isOK()) {
            return ret;
        }
        ret = toolsParsedOptions.validate();
        if (!ret.isOK()) {
            return ret;
        }
        return Status::OK();
    }

    MONGO_STARTUP_OPTIONS_STORE(BSONDumpOptions)(InitializerContext* context) {
        return storeBSONDumpOptions(toolsParsedOptions, context->args());
    }
}
