/**
 *    Copyright (C) 2025-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/replay/options_handler.h"

#include <filesystem>
#include <iostream>

#include <boost/filesystem/operations.hpp>
#include <boost/program_options.hpp>
#include <boost/program_options/errors.hpp>
#include <boost/program_options/options_description.hpp>
#include <boost/program_options/variables_map.hpp>
#include <fmt/format.h>


namespace mongo {


ReplayOptions OptionsHandler::handle(int argc, char** argv) {
    auto inputStr = "Path to file input file (defaults to stdin)";
    auto mongodbTarget = "URI of the shadow mongod/s";

    boost::program_options::variables_map vm;
    boost::program_options::options_description desc{"Options"};
    desc.add_options()("help,h",
                       "help")("input,i", boost::program_options::value<std::string>(), inputStr)(
        "target,t", boost::program_options::value<std::string>(), mongodbTarget);

    // Parse the program options
    store(parse_command_line(argc, argv, desc), vm);
    notify(vm);

    // Handle the help option
    if (vm.count("help") || vm.empty()) {
        std::cout << "MongoR Help: \n\n\t./mongor "
                     "-i trafficinput.txt -t <mongod/s uri> \n\n";
        std::cout << desc << std::endl;
        return {};
    }

    ReplayOptions options;

    // User can specify a --input param and it must point to a valid file
    if (vm.count("input")) {
        auto inputFile = vm["input"].as<std::string>();
        if (!std::filesystem::exists(inputFile)) {
            fmt::print(stderr, "Error: {} does not exist", inputFile);
            return {};
        }
        options.inputFile = inputFile;
    }

    if (vm.count("target")) {
        auto uri = vm["target"].as<std::string>();
        if (uri.empty()) {
            return {};
        }
        options.mongoURI = uri;
    }

    return options;
}
}  // namespace mongo
