/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <vector>

#include "mongo/db/service_context.h"
#include "mongo/db/wire_version.h"
#include "mongo/transport/transport_layer_manager.h"
#include "mongo/transport/transport_layer_manager_impl.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/testing_proctor.h"
#include "mongo/util/version.h"

#include "mock_version_info.h"
#include "testfile.h"

namespace queryTester {

namespace {
struct TestSpec {
    TestSpec(std::filesystem::path path,
             size_t low = 0,
             size_t high = std::numeric_limits<size_t>::max())
        : testPath(path), startTest(low), endTest(high){};

    // Validate that this test conforms to our expectations about filesystem things.
    void validate(ModeOption mode) const {
        uassert(9670437,
                mongo::str::stream{} << "Test file name must end in .test, not "
                                     << testPath.extension().string(),
                testPath.extension() == ".test" || mode == ModeOption::Normalize);
        uassert(9670438,
                mongo::str::stream{} << "Test file does not exist: " << testPath.string(),
                std::filesystem::exists(testPath));
        uassert(9670439,
                mongo::str::stream{}
                    << "A corresponding .results file must exist in compare mode for "
                    << testPath.string(),
                mode != ModeOption::Compare ||
                    std::filesystem::exists(
                        std::filesystem::path{testPath}.replace_extension(".results")));
    }

    std::filesystem::path testPath;
    size_t startTest;
    size_t endTest;
};
int exitWithError(const int statusCode, const std::string& msg) {
    std::cout << "Tester exiting after error: " << msg << "\n";
    exit(statusCode);
}

std::unique_ptr<mongo::DBClientConnection> buildConn(const std::string& uriString,
                                                     MockVersionInfo* const versionInfo,
                                                     const ModeOption mode) {
    if (mode == ModeOption::Normalize) {
        // Return a default (empty) unique ptr.
        return {};
    } else {
        // For now use a port not reserved for an MDB process.
        auto params = mongo::ServerGlobalParams{.port = 27016};
        mongo::VersionInfoInterface::enable(versionInfo);
        auto tl = mongo::transport::TransportLayerManagerImpl::createWithConfig(
            &params, mongo::getGlobalServiceContext());
        auto res = tl->setup();
        uassert(9670415, mongo::str::stream() << "Error setting up listener " << res, res.isOK());
        mongo::getGlobalServiceContext()->setTransportLayerManager(std::move(tl));
        auto mongoURI = mongo::MongoURI::parse(uriString);
        uassert(9670455,
                mongo::str::stream()
                    << "URI Parsing failed with message " << mongoURI.getStatus().reason(),
                mongoURI.isOK());
        auto conn = std::make_unique<mongo::DBClientConnection>(false, 0, mongoURI.getValue());
        auto hostAndPortVec = mongoURI.getValue().getServers();
        uassert(
            9670412, "Expected exactly one host/port in the given URI", hostAndPortVec.size() == 1);
        conn->connect(hostAndPortVec[0], "MongoTester", boost::none);
        return conn;
    }
}

int runTestProgram(const std::vector<TestSpec> testsToRun,
                   const std::string& uriString,
                   const QueryFile::CollectionInitOptions loadData,
                   const WriteOutOptions outOpt,
                   const ModeOption mode,
                   const bool populateAndExit) {
    // Enable required components.
    // TODO(DEVPROD-12295): Move to different function.
    auto versionInfo = MockVersionInfo();
    mongo::TestingProctor::instance().setEnabled(false);

    mongo::WireSpec::getWireSpec(mongo::getGlobalServiceContext())
        .initialize(mongo::WireSpec::Specification{});
    // Run the tests.
    auto conn = queryTester::buildConn(uriString, &versionInfo, mode);
    uassert(9670442,
            "Failed to connect to mongod in a connection-using mode.",
            conn || mode == ModeOption::Normalize);
    // Track collections loaded in the previous test file.
    auto prevFileCollections = std::set<std::string>{};
    // TODO(SERVER-96984): Robustify
    auto failedTestFiles = std::vector<std::filesystem::path>{};
    auto failedQueryCount = size_t{0};
    auto totalTestsRun = size_t{0};
    for (auto [testPath, startRange, endRange] : testsToRun) {
        auto currFile = queryTester::QueryFile(testPath, {startRange, endRange});
        currFile.readInEntireFile(mode);
        currFile.loadCollections(conn.get(), loadData, prevFileCollections);
        if (populateAndExit) {
            continue;
        }
        currFile.runTestFile(conn.get(), mode);
        totalTestsRun += currFile.getTestsRun();
        if (!currFile.writeAndValidate(mode, outOpt)) {
            failedQueryCount += currFile.getFailedQueryCount();
            failedTestFiles.push_back(testPath);
        }

        // Update prevFileCollections with the collections in the current file.
        prevFileCollections.clear();
        prevFileCollections.insert(currFile.getCollectionsNeeded().begin(),
                                   currFile.getCollectionsNeeded().end());
    }

    if (mode != ModeOption::Normalize) {
        conn->shutdown();
    }

    if (failedTestFiles.empty()) {
        std::cout << "All tests passed!\n";
        return 0;
    } else {
        fileHelpers::printFailureSummary(failedTestFiles, failedQueryCount, totalTestsRun);
        return 1;
    }
}
}  // namespace
}  // namespace queryTester

namespace {
void assertNextArgExists(const std::vector<std::string>& args,
                         const size_t& curArg,
                         const std::string& argName) {
    if (args.size() <= curArg + 1) {
        queryTester::exitWithError(1, std::string{"Expected more arguments after "} + argName);
    }
}

void printHelpString() {
    std::map<std::string, std::string> helpMap = {
        {"-h", "Print this help string"},
        {"--uri",
         "Follow with the mongo URI string to connect to a running "
         "mongo cluster. Required"},
        {"-t",
         "Test. This should be followed by a test name. This can appear "
         "multiple times to run multiple tests."},
        {"-n",  // TODO(DEVPROD-12147): Remove this option.
         "Run a specific test in the test file that immediately preceded "
         "this argument. This "
         "should be followed by an integer"},
        {"--load",
         "Load all collections specified in relevant test files. If "
         "not specified will assume data "
         "has already been loaded."},
        {"--drop",
         "Drop the collections before loading them. Should be "
         "specified with the load argument or "
         "no documents will exist in the test collections."},
        {"--out",
         "[result, oneline]. Write out results for each test file after running "
         "tests in run or normalize mode. Results files end in `.results` "
         "and will overwrite an existing file if it exists. "
         "`result` will write out multiline results, "
         "`oneline` will write out single-line results. "
         "Not available in compare mode."},
        {"--mode",
         "[run, compare, normalize]. Specify whether to just run and record "
         "results; expect all test files to specify results (default); or ensure that "
         "output results are correctly normalized."},
        {"-r",  // TODO(DEVPROD-12147): Remove this option.
         "Run a specific range of tests in the test file immediately "
         "before this argument. "
         "Should "
         "be followed by two integers in ascending order"},
        {"--populateAndExit", "Only drop and load data. No tests are run."}};
    for (const auto& [key, val] : helpMap) {
        std::cout << key << ": " << val << std::endl;
    }
}
}  // namespace

int main(const int argc, const char** const argv) {
    auto parsedArgs = std::vector<std::string>(argv, argv + argc);
    // Vector of file, startTest, endTest where the numbers are optional.
    auto testsToRun = std::vector<queryTester::TestSpec>{};
    auto expectingNumAt = size_t{0} - 1;  // TODO(DEVPROD-12147): Remove this value.
    auto runningPartialFile = false;
    auto loadOpt = queryTester::QueryFile::CollectionInitOptions::kNone;
    auto outOpt = queryTester::WriteOutOptions::kNone;
    auto mongoURIString = boost::optional<std::string>{};
    auto mode = queryTester::ModeOption::Compare;  // Default.
    auto populateAndExit = false;
    for (auto argNum = size_t{1}; argNum < parsedArgs.size(); ++argNum) {
        if (parsedArgs[argNum].compare("-t") == 0) {
            assertNextArgExists(parsedArgs, argNum, "-t");
            // The next -n we hit will modify the default test range of {0, max size_t} to be the
            // actual test number.
            testsToRun.push_back({parsedArgs[argNum + 1]});
            ++argNum;  // Skip the testName
            expectingNumAt = argNum + 1;

        } else if (parsedArgs[argNum].compare("-h") == 0) {
            printHelpString();
            break;
        } else if (parsedArgs[argNum].compare("-n") ==
                   0) {  // TODO(DEVPROD-12147): Remove this option.
            if (expectingNumAt != argNum) {
                queryTester::exitWithError(1, "-n must follow the -t test it modifies");
            }
            runningPartialFile = true;
            assertNextArgExists(parsedArgs, argNum, "-n");
            testsToRun.back().startTest = std::stoi(parsedArgs[argNum + 1]);
            testsToRun.back().endTest = std::stoi(parsedArgs[argNum + 1]);
            ++argNum;
        } else if (parsedArgs[argNum].compare("--out") == 0) {
            assertNextArgExists(parsedArgs, argNum, "--out");
            outOpt = queryTester::stringToWriteOutOpt(parsedArgs[argNum + 1]);
            ++argNum;
        } else if (parsedArgs[argNum].compare("-r") ==
                   0) {  // TODO(DEVPROD-12147): Remove this option.
            if (expectingNumAt != argNum) {
                queryTester::exitWithError(1, "-r must follow the -t test it modifies");
            }
            runningPartialFile = true;
            assertNextArgExists(parsedArgs, argNum, "-r");
            assertNextArgExists(parsedArgs, argNum + 1, "-r");
            testsToRun.back().startTest = std::stoi(parsedArgs[argNum + 1]);
            testsToRun.back().endTest = std::stoi(parsedArgs[argNum + 2]);
            if (testsToRun.back().startTest > testsToRun.back().endTest) {
                queryTester::exitWithError(
                    1,
                    mongo::str::stream()
                        << "Start test number must be lower than end test number for test "
                        << testsToRun.back().testPath.string());
            }
            argNum += 2;
        } else if (parsedArgs[argNum].compare("--uri") == 0) {
            assertNextArgExists(parsedArgs, argNum, "--uri");
            mongoURIString = parsedArgs[argNum + 1];
            ++argNum;
        } else if (parsedArgs[argNum].compare("--load") == 0) {
            if (loadOpt == queryTester::QueryFile::CollectionInitOptions::kNone) {
                loadOpt = queryTester::QueryFile::CollectionInitOptions::kLoad;
            } else {
                loadOpt = queryTester::QueryFile::CollectionInitOptions::kDropAndLoad;
            }
        } else if (parsedArgs[argNum].compare("--mode") == 0) {
            assertNextArgExists(parsedArgs, argNum, "--mode");
            mode = queryTester::stringToModeOption(parsedArgs[argNum + 1]);
            ++argNum;
        } else if (parsedArgs[argNum].compare("--drop") == 0) {
            if (loadOpt == queryTester::QueryFile::CollectionInitOptions::kNone) {
                loadOpt = queryTester::QueryFile::CollectionInitOptions::kDrop;
            } else {
                loadOpt = queryTester::QueryFile::CollectionInitOptions::kDropAndLoad;
            }
        } else if (parsedArgs[argNum].compare("--populateAndExit") == 0) {
            populateAndExit = true;
            loadOpt = queryTester::QueryFile::CollectionInitOptions::kDropAndLoad;
        } else {
            queryTester::exitWithError(1, std::string("Unexpected argument ") + parsedArgs[argNum]);
        }
    }
    if (!mongoURIString) {
        mongoURIString = "mongodb://localhost:27017";
        std::cout << "Using default URI of " << mongoURIString.get() << "\n";
    }

    if (runningPartialFile) {
        // Cannot write out if only running part of a file.
        if (outOpt != queryTester::WriteOutOptions::kNone) {
            queryTester::exitWithError(1, "--out not supported with either -n or -r");
        }
    }

    // Validate test file paths and compare values.
    for (const auto& testSpec : testsToRun) {
        testSpec.validate(mode);
    }

    uassert(9670441,
            "--mode compare and --out are incompatible.",
            outOpt == queryTester::WriteOutOptions::kNone ||
                mode != queryTester::ModeOption::Compare);

    try {
        auto serviceContextHolder = mongo::ServiceContext::make();
        setGlobalServiceContext(std::move(serviceContextHolder));
        return queryTester::runTestProgram(
            std::move(testsToRun), mongoURIString.get(), loadOpt, outOpt, mode, populateAndExit);
    } catch (mongo::AssertionException& ex) {
        queryTester::exitWithError(1, ex.reason());
    }
}
