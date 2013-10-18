/*
 *    Copyright (C) 2013 10gen Inc.
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

#include "mongo/tools/mongobridge_options.h"

#include "mongo/base/status.h"
#include "mongo/util/options_parser/startup_option_init.h"
#include "mongo/util/options_parser/startup_options.h"

namespace mongo {

    MongoBridgeGlobalParams mongoBridgeGlobalParams;

    typedef moe::OptionDescription OD;
    typedef moe::PositionalOptionDescription POD;

    Status addMongoBridgeOptions(moe::OptionSection* options) {

        Status ret = options->addOption(OD("help", "help", moe::Switch,
                    "produce help message", true));
        if(!ret.isOK()) {
            return ret;
        }

        ret = options->addOption(OD("port", "port", moe::Int, "port to listen for mongo messages"));
        if(!ret.isOK()) {
            return ret;
        }

        ret = options->addOption(OD("dest", "dest", moe::String, "uri of remote mongod instance"));
        if(!ret.isOK()) {
            return ret;
        }

        ret = options->addOption(OD("delay", "delay", moe::Int,
                    "transfer delay in milliseconds (default = 0)", true, moe::Value(0)));
        if(!ret.isOK()) {
            return ret;
        }

        return Status::OK();
    }

    void printMongoBridgeHelp(std::ostream* out) {
        *out << "Usage: mongobridge --port <port> --dest <dest> [ --delay <ms> ] [ --help ]"
             << std::endl;
        *out << moe::startupOptions.helpString();
        *out << std::flush;
    }

    bool handlePreValidationMongoBridgeOptions(const moe::Environment& params) {
        if (params.count("help")) {
            printMongoBridgeHelp(&std::cout);
            return true;
        }
        return false;
    }

    Status storeMongoBridgeOptions(const moe::Environment& params,
                                   const std::vector<std::string>& args) {

        if (!params.count("port")) {
            return Status(ErrorCodes::BadValue, "Missing required option: \"--port\"");
        }

        if (!params.count("dest")) {
            return Status(ErrorCodes::BadValue, "Missing required option: \"--dest\"");
        }

        mongoBridgeGlobalParams.port = params["port"].as<int>();
        mongoBridgeGlobalParams.destUri = params["dest"].as<std::string>();

        if (params.count("delay")) {
            mongoBridgeGlobalParams.delay = params["delay"].as<int>();
        }

        return Status::OK();
    }

    MONGO_GENERAL_STARTUP_OPTIONS_REGISTER(MongoBridgeOptions)(InitializerContext* context) {
        return addMongoBridgeOptions(&moe::startupOptions);
    }

    MONGO_STARTUP_OPTIONS_VALIDATE(MongoBridgeOptions)(InitializerContext* context) {
        if (handlePreValidationMongoBridgeOptions(moe::startupOptionsParsed)) {
            ::_exit(EXIT_SUCCESS);
        }
        Status ret = moe::startupOptionsParsed.validate();
        if (!ret.isOK()) {
            return ret;
        }
        return Status::OK();
    }

    MONGO_STARTUP_OPTIONS_STORE(MongoBridgeOptions)(InitializerContext* context) {
        Status ret = storeMongoBridgeOptions(moe::startupOptionsParsed, context->args());
        if (!ret.isOK()) {
            std::cerr << ret.toString() << std::endl;
            std::cerr << "try '" << context->args()[0] << " --help' for more information"
                      << std::endl;
            ::_exit(EXIT_BADOPTIONS);
        }
        return Status::OK();
    }
}

