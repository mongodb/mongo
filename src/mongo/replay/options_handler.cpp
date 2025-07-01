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

#include "mongo/base/error_codes.h"
#include "mongo/util/assert_util.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

#include <boost/filesystem/operations.hpp>
#include <boost/program_options.hpp>
#include <boost/program_options/errors.hpp>
#include <boost/program_options/options_description.hpp>
#include <boost/program_options/variables_map.hpp>
#include <fmt/format.h>
#include <yaml-cpp/yaml.h>

namespace mongo {

std::vector<ReplayOptions> OptionsHandler::handle(int argc, char** argv) {
    auto recordingPath = "Path to the recording file";
    auto mongodbTarget = "URI of the shadow mongod/s";
    auto configFilePath = "Path to the config file";

    namespace po = boost::program_options;

    po::variables_map vm;
    po::options_description desc{"Options"};
    // clang-format off
    desc.add_options()
        ("help,h", "help")
        ("input,i", po::value<std::string>(), recordingPath)
        ("target,t", po::value<std::string>(), mongodbTarget)
        ("config,c", po::value<std::string>(), configFilePath);
    // clang-format on

    // Parse the program options
    store(parse_command_line(argc, argv, desc), vm);
    notify(vm);

    // Handle the help option
    if (vm.count("help") || vm.empty()) {
        std::cout << "MongoR Help: \n\n"
                  << "\tUsage:\n"
                  << "\t\t./mongor -i <traffic input file> -t <mongod/s uri>\n"
                  << "\t\t./mongor -c <YAML config file path>\n\n"
                  << "\tConfig file format (YAML):\n"
                  << "\t\trecordings:\n"
                  << "\t\t  - path: \"recording_path1\"\n"
                  << "\t\t    uri: \"uri1\"\n"
                  << "\t\t  - path: \"recording_path2\"\n"
                  << "\t\t    uri: \"uri2\"\n\n";
        std::cout << desc << std::endl;
        return {};
    }

    const bool singleInstanceOptionsSpecified = vm.count("input") && vm.count("target");
    const bool configFileSpecified = vm.count("config");

    //-c <config file> has the precedence over -i -t options
    uassert(ErrorCodes::ReplayClientConfigurationError,
            "config file cannot be specified if single instance options are specified",
            singleInstanceOptionsSpecified ^ configFileSpecified);

    if (configFileSpecified) {
        // extract configuration file parameters.
        auto configPath = vm["config"].as<std::string>();
        uassert(ErrorCodes::ReplayClientConfigurationError,
                "config file path does not exist",
                std::filesystem::exists(configPath));
        return parseMultipleInstanceConfig(configPath);
    }

    // single instance bootstrapping. recording replayed against single server instance.
    uassert(ErrorCodes::ReplayClientConfigurationError,
            "target and input file must be specified for single instance configuration",
            singleInstanceOptionsSpecified);

    ReplayOptions options;
    // extract file path
    auto filePath = vm["input"].as<std::string>();
    uassert(ErrorCodes::ReplayClientConfigurationError,
            "input file path does not exist",
            std::filesystem::exists(filePath));
    options.recordingPath = filePath;
    // extract mongo uri
    auto uri = vm["target"].as<std::string>();
    uassert(ErrorCodes::ReplayClientConfigurationError, "target URI is empty", !uri.empty());
    options.mongoURI = uri;

    return {options};
}

std::vector<ReplayOptions> OptionsHandler::parseMultipleInstanceConfig(const std::string& path) {
    std::vector<ReplayOptions> parsedReplayOptions;
    uassert(ErrorCodes::ReplayClientConfigurationError,
            "Impossible to open config file",
            std::filesystem::exists(path));

    //   recordings:
    //      - path: "recording_path1"
    //        uri: "uri1"
    //      - path: "recording_path2"
    //        uri: "uri2"

    try {
        YAML::Node config = YAML::LoadFile(path);

        uassert(ErrorCodes::ReplayClientConfigurationError,
                "'recordings' key is missing",
                config["recordings"]);

        for (const auto& recordingNode : config["recordings"]) {
            uassert(ErrorCodes::ReplayClientConfigurationError,
                    "'recordings' key is missing",
                    recordingNode["path"] && recordingNode["uri"]);
            std::string filePath = recordingNode["path"].as<std::string>();
            std::string targetUri = recordingNode["uri"].as<std::string>();
            ReplayOptions replayOption = {filePath, targetUri};
            parsedReplayOptions.push_back(replayOption);
        }
    } catch (const YAML::Exception& ex) {
        uassert(ErrorCodes::ReplayClientConfigurationError, ex.what(), false);
    } catch (const std::exception& ex) {
        uassert(ErrorCodes::ReplayClientConfigurationError, ex.what(), false);
    }

    return parsedReplayOptions;
}

}  // namespace mongo
