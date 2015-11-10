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

#include "mongo/tools/mongobridge_options.h"

#include <cstdint>
#include <iostream>

#include "mongo/base/status.h"
#include "mongo/platform/random.h"
#include "mongo/util/options_parser/startup_options.h"

namespace mongo {

MongoBridgeGlobalParams mongoBridgeGlobalParams;

Status addMongoBridgeOptions(moe::OptionSection* options) {
    options->addOptionChaining("help", "help", moe::Switch, "show this usage information");

    options->addOptionChaining("port", "port", moe::Int, "port to listen on for MongoDB messages");

    options->addOptionChaining("seed", "seed", moe::Long, "random seed to use");

    options->addOptionChaining("dest", "dest", moe::String, "URI of remote MongoDB process");

    return Status::OK();
}

void printMongoBridgeHelp(std::ostream* out) {
    *out << "Usage: mongobridge --port <port> --dest <dest> [ --seed <seed> ] [ --help ]"
         << std::endl;
    *out << moe::startupOptions.helpString();
    *out << std::flush;
}

bool handlePreValidationMongoBridgeOptions(const moe::Environment& params) {
    if (params.count("help")) {
        printMongoBridgeHelp(&std::cout);
        return false;
    }
    return true;
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

    if (!params.count("seed")) {
        std::unique_ptr<SecureRandom> seedSource{SecureRandom::create()};
        mongoBridgeGlobalParams.seed = seedSource->nextInt64();
    } else {
        mongoBridgeGlobalParams.seed = static_cast<int64_t>(params["seed"].as<long>());
    }

    return Status::OK();
}

}  // namespace mongo
