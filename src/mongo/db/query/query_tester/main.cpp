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
#include "mongo/util/shell_exec.h"
#include "mongo/util/testing_proctor.h"
#include "mongo/util/version.h"

#include "mock_version_info.h"
#include "testfile.h"

namespace mongo::query_tester {
namespace {
struct TestSpec {
    TestSpec(std::filesystem::path path, size_t low = kMinTestNum, size_t high = kMaxTestNum)
        : testPath(path), startTest(low), endTest(high){};

    // Validate that this test conforms to our expectations about filesystem things.
    void validate(ModeOption mode) const {
        uassert(9670437,
                str::stream{} << "Test file name must end in .test, not "
                              << testPath.extension().string(),
                testPath.extension() == ".test" || mode == ModeOption::Normalize);
        uassert(9670438,
                str::stream{} << "Test file does not exist: " << testPath.string(),
                std::filesystem::exists(testPath));
        uassert(9670439,
                str::stream{} << "A corresponding .results file must exist in compare mode for "
                              << testPath.string(),
                mode != ModeOption::Compare ||
                    std::filesystem::exists(
                        std::filesystem::path{testPath}.replace_extension(".results")));
    }

    std::filesystem::path testPath;
    size_t startTest;
    size_t endTest;
};

std::unique_ptr<DBClientConnection> buildConn(const std::string& uriString,
                                              MockVersionInfo* const versionInfo,
                                              const ModeOption mode) {
    if (mode == ModeOption::Normalize) {
        // Return a default (empty) unique ptr.
        return {};
    } else {
        // Enable required components.
        TestingProctor::instance().setEnabled(false);

        WireSpec::getWireSpec(getGlobalServiceContext()).initialize(WireSpec::Specification{});

        // For now use a port not reserved for an MDB process.
        auto params = ServerGlobalParams{.port = 27016};
        VersionInfoInterface::enable(versionInfo);
        auto tl = transport::TransportLayerManagerImpl::createWithConfig(&params,
                                                                         getGlobalServiceContext());
        auto res = tl->setup();
        uassert(9670415, str::stream{} << "Error setting up listener " << res, res.isOK());
        getGlobalServiceContext()->setTransportLayerManager(std::move(tl));
        auto mongoURI = MongoURI::parse(uriString);
        uassert(9670455,
                str::stream{} << "URI Parsing failed with message "
                              << mongoURI.getStatus().reason(),
                mongoURI.isOK());
        auto conn = std::make_unique<DBClientConnection>(false, 0, mongoURI.getValue());
        auto hostAndPortVec = mongoURI.getValue().getServers();
        uassert(
            9670412, "Expected exactly one host/port in the given URI", hostAndPortVec.size() == 1);
        conn->connect(hostAndPortVec[0], "MongoTester", boost::none);
        uassert(9699400, "Failed to connect to mongod in a connection-using mode.", conn);
        return conn;
    }
}

void exitWithError(const int statusCode, const std::string& msg) {
    std::cerr << "Tester exiting after error: " << msg << std::endl;
    std::exit(statusCode);
}

int runTestProgram(const std::vector<TestSpec> testsToRun,
                   const std::string& uriString,
                   const bool dropData,
                   const bool loadData,
                   const WriteOutOptions outOpt,
                   const ModeOption mode,
                   const bool populateAndExit,
                   const bool verbose) {
    // Run the tests.
    auto versionInfo = MockVersionInfo{};
    auto conn = buildConn(uriString, &versionInfo, mode);
    // Track collections loaded in the previous test file.
    auto prevFileCollections = std::set<std::string>{};
    // TODO(SERVER-96984): Robustify
    auto failedTestFiles = std::vector<std::filesystem::path>{};
    auto failedQueryCount = size_t{0};
    auto totalTestsRun = size_t{0};
    for (const auto& [testPath, startRange, endRange] : testsToRun) {
        auto currFile = query_tester::QueryFile(testPath);
        currFile.readInEntireFile(mode, startRange, endRange);
        currFile.loadCollections(conn.get(), dropData, loadData, prevFileCollections);
        if (populateAndExit) {
            continue;
        }
        currFile.runTestFile(conn.get(), mode);
        totalTestsRun += currFile.getTestsRun();
        if (!currFile.writeAndValidate(mode, outOpt, verbose)) {
            failedQueryCount += currFile.getFailedQueryCount();
            failedTestFiles.push_back(testPath);
        }

        // Update prevFileCollections with the collections in the current file.
        prevFileCollections.clear();
        prevFileCollections.insert(currFile.getCollectionsNeeded().begin(),
                                   currFile.getCollectionsNeeded().end());
    }

    if (populateAndExit) {
        std::cout << std::endl << "Documents and indexes loaded from collection!" << std::endl;
        return 0;
    }
    if (mode != ModeOption::Normalize) {
        conn->shutdown();
    }

    if (failedTestFiles.empty()) {
        std::cout << std::endl << "All tests passed!" << std::endl;
        return 0;
    } else {
        printFailureSummary(failedTestFiles, failedQueryCount, totalTestsRun);
        if (verbose) {
            const auto pyCmd = std::stringstream{}
                << "python3 src/mongo/db/query/query_tester/scripts/extract_pickle_to_json.py "
                << getMongoRepoRoot() << " " << kFeatureExtractorDir << " " << kTmpFailureFile;
            const auto queryFeaturesFile =
                std::filesystem::path{(std::stringstream{} << "src/mongo/db/query/query_tester/"
                                                           << kTmpFailureFile << ".json")
                                          .str()};
            if (shellExec(pyCmd.str(), kShellTimeout, kShellMaxLen, true).isOK()) {
                displayFailingQueryFeatures(queryFeaturesFile);
            } else {
                exitWithError(1, "failed to extract pickle file to json for feature processing.");
            }

            std::filesystem::remove(queryFeaturesFile);
        }
        return 1;
    }
}

void assertNextArgExists(const std::vector<std::string>& args,
                         const size_t& curArg,
                         const std::string& argName) {
    if (args.size() <= curArg + 1) {
        exitWithError(1, std::string{"Expected more arguments after "} + argName);
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
        {"-n",
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
        {"-r",
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

int queryTesterMain(const int argc, const char** const argv) {
    auto parsedArgs = std::vector<std::string>(argv, argv + argc);
    // Vector of file, startTest, endTest where the numbers are optional.
    auto testsToRun = std::vector<TestSpec>{};
    auto expectingNumAt = size_t{0} - 1;
    auto runningPartialFile = false;
    auto dropOpt = false;
    auto loadOpt = false;
    auto outOpt = WriteOutOptions::kNone;
    auto mongoURIString = boost::optional<std::string>{};
    auto mode = ModeOption::Compare;  // Default.
    auto populateAndExit = false;
    auto verbose = false;
    for (auto argNum = size_t{1}; argNum < parsedArgs.size(); ++argNum) {
        // Same order as in the help menu.
        if (parsedArgs[argNum] == "--drop") {
            dropOpt = true;
        } else if (parsedArgs[argNum] == "--load") {
            loadOpt = true;
        } else if (parsedArgs[argNum] == "--mode") {
            assertNextArgExists(parsedArgs, argNum, "--mode");
            mode = stringToModeOption(parsedArgs[argNum + 1]);
            ++argNum;
        } else if (parsedArgs[argNum] == "--out") {
            assertNextArgExists(parsedArgs, argNum, "--out");
            outOpt = stringToWriteOutOpt(parsedArgs[argNum + 1]);
            ++argNum;
        } else if (parsedArgs[argNum] == "--populateAndExit") {
            std::tie(dropOpt, loadOpt, populateAndExit) = std::tuple{true, true, true};
        } else if (parsedArgs[argNum] == "--uri") {
            assertNextArgExists(parsedArgs, argNum, "--uri");
            mongoURIString = parsedArgs[argNum + 1];
            ++argNum;
        } else if (parsedArgs[argNum] == "-h") {
            printHelpString();
            std::exit(0);
        } else if (parsedArgs[argNum] == "-n") {
            if (expectingNumAt != argNum) {
                exitWithError(1, "-n must follow the -t test it modifies");
            }
            runningPartialFile = true;
            assertNextArgExists(parsedArgs, argNum, "-n");
            testsToRun.back().startTest = std::stoull(parsedArgs[argNum + 1]);
            testsToRun.back().endTest = std::stoull(parsedArgs[argNum + 1]);
            ++argNum;
        } else if (parsedArgs[argNum] == "-r") {
            if (expectingNumAt != argNum) {
                exitWithError(1, "-r must follow the -t test it modifies");
            }
            runningPartialFile = true;
            assertNextArgExists(parsedArgs, argNum, "-r");
            assertNextArgExists(parsedArgs, argNum + 1, "-r");
            testsToRun.back().startTest = std::stoull(parsedArgs[argNum + 1]);
            testsToRun.back().endTest = std::stoull(parsedArgs[argNum + 2]);
            if (testsToRun.back().startTest > testsToRun.back().endTest) {
                exitWithError(
                    1,
                    str::stream{}
                        << "Start test number must be lower than end test number for test "
                        << testsToRun.back().testPath.string());
            }
            argNum += 2;
        } else if (parsedArgs[argNum] == "-t") {
            assertNextArgExists(parsedArgs, argNum, "-t");
            // The next -n we hit will modify the default test range of {0, max size_t} to be the
            // actual test number.
            testsToRun.push_back({parsedArgs[argNum + 1]});
            ++argNum;  // Skip the testName
            expectingNumAt = argNum + 1;
        } else if (parsedArgs[argNum] == "-v") {
            verbose = true;
        } else {
            exitWithError(1, std::string{"Unexpected argument "} + parsedArgs[argNum]);
        }
    }

    if (!mongoURIString) {
        mongoURIString = "mongodb://localhost:27017";
        std::cout << "Using default URI of " << mongoURIString.get() << std::endl;
    }

    // Validate some flag conditions.
    for (const auto& [condition, message] : std::map<bool, std::string>{
             {mode == ModeOption::Compare && outOpt != WriteOutOptions::kNone,
              "--mode compare and --out are incompatible."},
             // Cannot write out if only running part of a file.
             {runningPartialFile && outOpt != WriteOutOptions::kNone,
              "--out not supported with either -n or -r"},
             {mode == ModeOption::Normalize && (dropOpt || loadOpt),
              "--drop and --load are incompatible with --mode normalize."},
             {populateAndExit && testsToRun.size() != 1,
              "--populateAndExit must be specified with a single test file."},
             {testsToRun.empty(), "Make sure to provide QueryTester with a .test file."},
             {verbose && mode != ModeOption::Compare,
              "option -v must be specified with --mode compare."}}) {
        if (condition) {
            exitWithError(1, message);
        }
    }

    // Validate test file paths and compare values.
    for (const auto& testSpec : testsToRun) {
        testSpec.validate(mode);
    }

    try {
        auto serviceContextHolder = ServiceContext::make();
        setGlobalServiceContext(std::move(serviceContextHolder));
        return runTestProgram(std::move(testsToRun),
                              mongoURIString.get(),
                              dropOpt,
                              loadOpt,
                              outOpt,
                              mode,
                              populateAndExit,
                              verbose);
    } catch (AssertionException& ex) {
        exitWithError(1, ex.reason());
    }
    MONGO_UNREACHABLE;
}
}  // namespace mongo::query_tester

int main(const int argc, const char** const argv) {
    return mongo::query_tester::queryTesterMain(argc, argv);
}
