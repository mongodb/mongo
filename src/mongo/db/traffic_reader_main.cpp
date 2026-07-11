// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include <fcntl.h>

#include <boost/filesystem/operations.hpp>
#include <boost/program_options/value_semantic.hpp>
// IWYU pragma: no_include "boost/program_options/detail/parsers.hpp"
#include <cerrno>
#include <cstring>
#include <fstream>  // IWYU pragma: keep
#include <iostream>
#include <string>
#include <vector>

#include <boost/program_options/errors.hpp>
#include <boost/program_options/options_description.hpp>
#include <boost/program_options/variables_map.hpp>
#include <boost/type_index/type_index_facade.hpp>

#ifdef _WIN32
#include <io.h>
#endif

#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/db/traffic_reader.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/signal_handlers.h"
#include "mongo/util/text.h"  // IWYU pragma: keep

#include <boost/program_options.hpp>  // IWYU pragma: keep

using namespace mongo;

int main(int argc, char* argv[]) {

    setupSignalHandlers();

    Status status = mongo::runGlobalInitializers(std::vector<std::string>(argv, argv + argc));
    if (!status.isOK()) {
        std::cerr << "Failed global initialization: " << status << std::endl;
        return static_cast<int>(ExitCode::fail);
    }

    startSignalProcessingThread();

    // Handle program options
    boost::program_options::variables_map vm;

    // input / output files for the reader (input defaults to stdin)
    int inputFd = 0;
    std::ofstream outputStream;

    try {
        // Define the program options
        auto inputStr = "Path to file input file (defaults to stdin)";
        auto outputStr =
            "Path to file that mongotrafficreader will place its output (defaults to stdout)";
        boost::program_options::options_description desc{"Options"};
        desc.add_options()("help,h", "help")(
            "input,i", boost::program_options::value<std::string>(), inputStr)(
            "output,o", boost::program_options::value<std::string>(), outputStr);

        // Parse the program options
        store(parse_command_line(argc, argv, desc), vm);
        notify(vm);

        // Handle the help option
        if (vm.count("help")) {
            std::cout << "Mongo Traffic Reader Help: \n\n\t./mongotrafficreader "
                         "-i trafficinput.txt -o mongotrafficreader_dump.bson \n\n"
                      << desc << std::endl;
            return static_cast<int>(ExitCode::clean);
        }

        // User can specify a --input param and it must point to a valid file
        if (vm.count("input")) {
            auto inputFile = vm["input"].as<std::string>();
            if (!boost::filesystem::exists(inputFile.c_str())) {
                std::cout << "Error: Specified file does not exist (" << inputFile.c_str() << ")"
                          << std::endl;
                return static_cast<int>(ExitCode::fail);
            }

// Open the connection to the input file
#ifdef _WIN32
            inputFd = open(inputFile.c_str(), O_RDONLY | O_BINARY);
#else
            inputFd = open(inputFile.c_str(), O_RDONLY);
#endif

            if (inputFd < 0) {
                std::cerr << "Error opening file: " << strerror(errno) << std::endl;
                return static_cast<int>(ExitCode::fail);
            }
        }

        // User must specify a --output param and it does not need to point to a valid file
        if (vm.count("output")) {
            auto outputFile = vm["output"].as<std::string>();

            // Open the connection to the output file
            outputStream.open(outputFile, std::ios::out | std::ios::trunc | std::ios::binary);
            if (!outputStream.is_open()) {
                std::cerr << "Error writing to file: " << outputFile << std::endl;
                return static_cast<int>(ExitCode::fail);
            }
        } else {
            // output to std::cout
            outputStream.copyfmt(std::cout);
            outputStream.clear(std::cout.rdstate());
            outputStream.basic_ios<char>::rdbuf(std::cout.rdbuf());
        }
    } catch (const boost::program_options::error& ex) {
        std::cerr << ex.what() << '\n';
        return static_cast<int>(ExitCode::fail);
    }

    mongo::trafficRecordingFileToMongoReplayFile(inputFd, outputStream);

    return 0;
}
