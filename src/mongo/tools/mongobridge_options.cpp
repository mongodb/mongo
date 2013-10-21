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
#include "mongo/util/options_parser/startup_options.h"

namespace mongo {

    MongoBridgeGlobalParams mongoBridgeGlobalParams;

    typedef moe::OptionDescription OD;
    typedef moe::PositionalOptionDescription POD;

    Status addMongoBridgeOptions(moe::OptionSection* options) {

        options->addOptionChaining("help", "help", moe::Switch, "produce help message");


        options->addOptionChaining("port", "port", moe::Int, "port to listen for mongo messages");


        options->addOptionChaining("dest", "dest", moe::String, "uri of remote mongod instance");


        options->addOptionChaining("delay", "delay", moe::Int,
                "transfer delay in milliseconds (default = 0)")
                                  .setDefault(moe::Value(0));


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

} // namespace mongo
