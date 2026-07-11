// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/client/sdam/json_test_runner_cli_options_gen.h"

#include <cstdlib>
#include <fstream>  // IWYU pragma: keep
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/format.hpp>


namespace mongo::sdam {

class ArgParser {
public:
    ArgParser(int argc, char* argv[]);

    void LogParams() const;

    const std::string& SourceDirectory() const {
        return _sourceDirectory;
    }

    const std::vector<std::string>& TestFilters() const {
        return _testFilters;
    }

    int Verbose() const {
        return _verbose;
    }

private:
    constexpr static auto kSourceDirOption = "source-dir";
    constexpr static auto kSourceDirDefault = ".";

    constexpr static auto kFilterOption = "filter";

    constexpr static int kHelpExitCode = 0;
    constexpr static int kArgParseExitCode = 1024;

    constexpr static auto kVerbose = "verbose";

    std::string _sourceDirectory = kSourceDirDefault;
    std::vector<std::string> _testFilters;
    int _verbose = 0;

    void printHelpAndExit(char* programName, const std::string desc) {
        std::cout << programName << ":" << std::endl << desc << std::endl;
        std::exit(kHelpExitCode);
    }

    std::vector<std::string> toStringVector(int n, char** array) {
        std::vector<std::string> result;
        for (int i = 0; i < n; ++i)
            result.push_back(array[i]);
        return result;
    }
};
};  // namespace mongo::sdam
