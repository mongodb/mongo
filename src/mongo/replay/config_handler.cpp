// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/replay/config_handler.h"

#include "mongo/base/error_codes.h"
#include "mongo/util/assert_util.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <absl/time/time.h>
#include <boost/filesystem/operations.hpp>
#include <boost/optional/optional.hpp>
#include <boost/program_options.hpp>
#include <boost/program_options/errors.hpp>
#include <boost/program_options/options_description.hpp>
#include <boost/program_options/variables_map.hpp>
#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <yaml-cpp/yaml.h>

using json = nlohmann::json;

namespace mongo {

std::vector<ReplayConfig> ConfigHandler::parse(int argc, char** argv) {
    auto recordingPath = "Path to the recording file";
    auto mongodbTarget = "URI of the shadow mongod/s";
    auto configFilePath = "Path to the config file";
    auto enablePerfRecording = "Enable/Disable perf recording specifying file name";
    auto graceTime =
        "Delay replay to allow time for session initialization before the first event for that "
        "session";

    namespace po = boost::program_options;

    po::variables_map vm;
    po::options_description desc{"Options"};
    // clang-format off
    desc.add_options()
        ("help,h", "help")
        ("input,i", po::value<std::string>(), recordingPath)
        ("target,t", po::value<std::string>(), mongodbTarget)
        ("config,c", po::value<std::string>(), configFilePath)
        ("perf,p", po::value<std::string>()->default_value(""), enablePerfRecording)
        ("graceTime,a", po::value<std::string>()->default_value("5s"), graceTime);
    // clang-format on

    // Parse the program options
    store(parse_command_line(argc, argv, desc), vm);
    notify(vm);

    // Handle the help option
    if (vm.count("help") || vm.empty()) {
        std::cout << "MongoR Help: \n\n"
                  << "\tUsage:\n"
                  << "\t\t./mongor -i <traffic input file> -t <mongod/s uri> -p <filename for "
                     "enabling perf reporting>\n"
                  << "\t\t./mongor -c <JSON config file path>\n\n"
                  << "\tConfig file format (JSON):\n"
                  << "\t\t{\n"
                  << "\t\t\trecordings: [\n"
                  << "\t\t\t\t{\n"
                  << "\t\t\t\t\t\"path\": \"recording_path1\",\n"
                  << "\t\t\t\t\t\"uri\": \"uri1\"\n"
                  << "\t\t\t\t},\n"
                  << "\t\t\t\t{\n"
                  << "\t\t\t\t\t\"path\": \"recording_path2\",\n"
                  << "\t\t\t\t\t\"uri\": \"uri2\"\n"
                  << "\t\t\t\t}\n"
                  << "\t\t\t[\n"
                  << "\t\t}\n";
        std::cout << desc << std::endl;
        return {};
    }

    const bool singleInstanceOptionsSpecified = vm.count("input") && vm.count("target");
    const bool configFileSpecified = vm.count("config");

    //-c <config file> has the precedence over -i -t options
    uassert(ErrorCodes::ReplayClientConfigurationError,
            "The configuration provided is invalid.",
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

    ReplayConfig config;
    // extract file path
    auto filePath = vm["input"].as<std::string>();
    uassert(ErrorCodes::ReplayClientConfigurationError,
            "input file path does not exist",
            std::filesystem::exists(filePath));
    config.recordingPath = filePath;
    // extract mongo uri
    auto uri = vm["target"].as<std::string>();
    uassert(ErrorCodes::ReplayClientConfigurationError, "target URI is empty", !uri.empty());
    config.mongoURI = uri;
    config.enablePerformanceRecording = vm["perf"].as<std::string>();

    absl::Duration dur;
    uassert(ErrorCodes::ReplayClientConfigurationError,
            "Invalid graceTime",
            absl::ParseDuration(vm["graceTime"].as<std::string>(), &dur));
    uassert(ErrorCodes::ReplayClientConfigurationError,
            "Invalid graceTime",
            dur >= absl::ZeroDuration());
    config.sessionPreInitTime = absl::ToChronoSeconds(dur);
    return {config};
}

std::vector<ReplayConfig> ConfigHandler::parseMultipleInstanceConfig(const std::string& path) {
    std::vector<ReplayConfig> configurations;
    uassert(ErrorCodes::ReplayClientConfigurationError,
            "Impossible to open config file",
            std::filesystem::exists(path));

    // {
    //     recordings: [
    //         {
    //             "path": "recording_path1",
    //             "uri": "uri1"
    //         },
    //         {
    //             "path": "recording_path2",
    //             "uri": "uri2"
    //         }
    //     ]
    // }

    try {

        std::ifstream configFile;
        configFile.open(path);

        json config;
        configFile >> config;

        boost::optional<std::chrono::seconds> graceTime;

        if (config.contains("graceTime")) {
            absl::Duration dur;
            uassert(ErrorCodes::ReplayClientConfigurationError,
                    "Invalid graceTime",
                    absl::ParseDuration(config["graceTime"].get<std::string>(), &dur));

            uassert(ErrorCodes::ReplayClientConfigurationError,
                    "Invalid graceTime",
                    dur >= absl::ZeroDuration());
            graceTime = absl::ToChronoSeconds(dur);
        }

        uassert(ErrorCodes::ReplayClientConfigurationError,
                "'recordings' key is missing",
                config.contains("recordings"));
        uassert(ErrorCodes::ReplayClientConfigurationError,
                "'recordings' key must contain an array",
                config["recordings"].is_array());

        for (const auto& recording : config["recordings"]) {
            uassert(ErrorCodes::ReplayClientConfigurationError,
                    "'path' and 'uri' keys are required in each recording",
                    recording.contains("path") && recording.contains("uri"));
            std::string filePath = recording["path"].get<std::string>();
            std::string targetUri = recording["uri"].get<std::string>();
            ReplayConfig replayConfig = {filePath, targetUri};
            if (graceTime) {
                replayConfig.sessionPreInitTime = *graceTime;
            }
            configurations.push_back(std::move(replayConfig));
        }

    } catch (const json::exception& ex) {
        uassert(ErrorCodes::ReplayClientConfigurationError, ex.what(), false);
    } catch (const std::exception& ex) {
        uassert(ErrorCodes::ReplayClientConfigurationError, ex.what(), false);
    }

    return configurations;
}

}  // namespace mongo
