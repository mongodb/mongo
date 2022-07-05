/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <string>

#ifdef _WIN32
#include <io.h>
#endif

#include "mongo/base/initializer.h"
#include "mongo/db/traffic_reader.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/signal_handlers.h"
#include "mongo/util/text.h"

#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>

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
