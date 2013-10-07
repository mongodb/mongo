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

#include "mongo/tools/mongotop_options.h"

#include "mongo/base/status.h"
#include "mongo/util/options_parser/environment.h"
#include "mongo/util/options_parser/option_description.h"
#include "mongo/util/options_parser/option_section.h"
#include "mongo/util/options_parser/options_parser.h"
#include "mongo/util/options_parser/startup_option_init.h"

namespace mongo {

    MongoTopGlobalParams mongoTopGlobalParams;

    typedef moe::OptionDescription OD;
    typedef moe::PositionalOptionDescription POD;

    Status addMongoTopOptions(moe::OptionSection* options) {
        Status ret = addGeneralToolOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        ret = addRemoteServerToolOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        ret = options->addOption(OD("locks", "locks", moe::Switch,
                    "use db lock info instead of top", true));
        if(!ret.isOK()) {
            return ret;
        }

        ret = options->addPositionalOption(POD( "sleep", moe::Int, 1 ));
        if(!ret.isOK()) {
            return ret;
        }

        return Status::OK();
    }

    void printMongoTopHelp(const moe::OptionSection options, std::ostream* out) {
        *out << "View live MongoDB collection statistics.\n" << std::endl;
        *out << options.helpString();
        *out << std::flush;
    }

    Status handlePreValidationMongoTopOptions(const moe::Environment& params) {
        if (toolsParsedOptions.count("help")) {
            printMongoTopHelp(toolsOptions, &std::cout);
            ::_exit(0);
        }
        return Status::OK();
    }

    Status storeMongoTopOptions(const moe::Environment& params,
                                const std::vector<std::string>& args) {
        Status ret = storeGeneralToolOptions(params, args);
        if (!ret.isOK()) {
            return ret;
        }

        mongoTopGlobalParams.sleep = getParam("sleep", 1);
        mongoTopGlobalParams.useLocks = hasParam("locks");

        // Make the default db "admin" if it was not explicitly set
        if (!toolsParsedOptions.count("db")) {
            toolGlobalParams.db = "admin";
        }

        return Status::OK();
    }

    MONGO_GENERAL_STARTUP_OPTIONS_REGISTER(MongoTopOptions)(InitializerContext* context) {
        return addMongoTopOptions(&toolsOptions);
    }

    MONGO_STARTUP_OPTIONS_PARSE(MongoTopOptions)(InitializerContext* context) {
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

    MONGO_STARTUP_OPTIONS_VALIDATE(MongoTopOptions)(InitializerContext* context) {
        Status ret = handlePreValidationMongoTopOptions(toolsParsedOptions);
        if (!ret.isOK()) {
            return ret;
        }
        ret = toolsParsedOptions.validate();
        if (!ret.isOK()) {
            return ret;
        }
        return Status::OK();
    }

    MONGO_STARTUP_OPTIONS_STORE(MongoTopOptions)(InitializerContext* context) {
        return storeMongoTopOptions(toolsParsedOptions, context->args());
    }
}
