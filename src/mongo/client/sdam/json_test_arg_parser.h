/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include <fstream>
#include <iostream>
#include <memory>

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/format.hpp>
#include <boost/optional/optional_io.hpp>

#include "mongo/client/sdam/json_test_runner_cli_options_gen.h"


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
