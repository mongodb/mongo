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

#include "mongo/db/query/query_tester/command_helpers.h"
#include "mongo/db/query/query_tester/mock_version_info.h"
#include "mongo/db/query/query_tester/testfile.h"
#include "mongo/db/service_context.h"
#include "mongo/db/wire_version.h"
#include "mongo/platform/random.h"
#include "mongo/transport/transport_layer_manager.h"
#include "mongo/transport/transport_layer_manager_impl.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/shell_exec.h"
#include "mongo/util/testing_proctor.h"
#include "mongo/util/version.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <vector>

namespace mongo::query_tester {
namespace {
struct TestSpec {
    TestSpec(std::filesystem::path path, size_t low = kMinTestNum, size_t high = kMaxTestNum)
        : testPath(path), startTest(low), endTest(high) {};

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

void attemptToSetTransportLayerManager() {
    auto params = ServerGlobalParams{.port = 0, .noUnixSocket = true};
    auto tl =
        transport::TransportLayerManagerImpl::createWithConfig(&params, getGlobalServiceContext());
    auto res = tl->setup();

    uassert(9670415, str::stream{} << "Error setting up listener: " << res, res.isOK());

    getGlobalServiceContext()->setTransportLayerManager(std::move(tl));
}

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
        VersionInfoInterface::enable(versionInfo);

        attemptToSetTransportLayerManager();

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
                   const bool createAllIndices,
                   const bool ignoreIndexFailures,
                   const WriteOutOptions outOpt,
                   const ModeOption mode,
                   const bool optimizationsOff,
                   const bool populateAndExit,
                   const ErrorLogLevel errorLogLevel,
                   const DiffStyle diffStyle,
                   const OverrideOption overrideOption) {
    // Run the tests.
    auto versionInfo = MockVersionInfo{};
    auto conn = buildConn(uriString, &versionInfo, mode);

    // Append _id to setWindowFields sort to make queries deterministic.
    auto deterministicSetWindowFields =
        fromFuzzerJson("{setParameter: 1, internalQueryAppendIdToSetWindowFieldsSort: true}");
    runCommandAssertOK(conn.get(), deterministicSetWindowFields, "admin");

    // Track collections loaded in the previous test file.
    auto prevFileCollections = std::set<CollectionSpec>{};
    auto failedTestFiles = std::vector<std::filesystem::path>{};
    auto failedQueryCount = size_t{0};
    auto totalTestsRun = size_t{0};
    for (const auto& [testPath, startRange, endRange] : testsToRun) {
        auto currFile = query_tester::QueryFile(testPath, optimizationsOff, overrideOption);

        // Treat data load errors as failures, too.
        try {
            currFile.readInEntireFile(mode, startRange, endRange);
            currFile.loadCollections(conn.get(),
                                     dropData,
                                     loadData,
                                     createAllIndices,
                                     ignoreIndexFailures,
                                     prevFileCollections);
        } catch (const std::exception& exception) {
            std::cerr << std::endl
                      << testPath.string() << std::endl
                      << exception.what() << std::endl;
            failedTestFiles.push_back(testPath);
            prevFileCollections.clear();  // Assume data corruption on data load failure.
            continue;
        }

        if (populateAndExit) {
            continue;
        }

        // Treat run errors as failures, but since data load is fine, we can still make use of the
        // drop-load optimization.
        const bool hasFailures = [&](const auto& testPath) {
            try {
                currFile.runTestFile(conn.get(), mode);
                const bool isPartialRun = (startRange != kMinTestNum || endRange != kMaxTestNum);
                return !currFile.writeAndValidate(
                    mode, outOpt, errorLogLevel, diffStyle, isPartialRun);
            } catch (const std::exception& exception) {
                std::cerr << std::endl
                          << testPath.string() << std::endl
                          << exception.what() << std::endl;
                return true;
            }
        }(testPath);

        totalTestsRun += currFile.getTestsRun();
        if (hasFailures) {
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
        if (errorLogLevel == ErrorLogLevel::kSimple) {
            std::cout
                << "Tests failed! Run with -v and optionally --extractFeatures for more details."
                << std::endl;
        } else {
            printFailureSummary(failedTestFiles, failedQueryCount, totalTestsRun);

            if (errorLogLevel == ErrorLogLevel::kVerbose) {
                std::cout
                    << "Tests failed! Run test locally with --extractFeatures for more details."
                    << std::endl;
            }
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
    static const auto kHelpMap = std::map<std::string, std::string>{
        // Long-options come before short-options. Sorted in lexicographical order.
        {"--diff",
         "[plain, word]. Controls how result differences are displayed. `word` (default) shows "
         "colored word-level diffs — recommended for ANSI-capable terminals. `plain` shows "
         "uncolored line-level diffs."},
        {"--drop",
         "Drop the test collections before loading them. Almost always paired with --load; "
         "without it the collections will be empty after the drop."},
        {"--extractFeatures",
         "For each failed query, extract syntax, query planner, and execution stats metadata to "
         "aid debugging. Requires -v and --mode compare. The feature-extractor tool must be "
         "present in the user's home directory."},
        {"--ignore-index-failures",
         "Suppress errors from index creation failures and continue loading. Useful when running "
         "against a server version that does not support all index types in the test collection."},
        {"--load",
         "Insert documents and build indexes for all collections referenced by the test files. "
         "If omitted, the collections are assumed to already contain the correct data."},
        {"--minimal-index",
         "Skip non-essential index creation and only build indexes required for queries to run "
         "at all (currently: geospatial and text indexes). Speeds up collection loading at the "
         "cost of missing index coverage."},
        {"--mode",
         "[run, compare, normalize]. `compare` (default): run each test and fail if results "
         "differ from the .results file. `run`: execute tests and optionally write output via "
         "--out — tests only fail on execution errors. `normalize`: validate that existing "
         ".results files are in canonical form without connecting to a server."},
        {"--opt-off",
         "Disable query optimizations and find-layer pushdown. Used to generate a baseline "
         "results file for differential/multiversion testing. Requires "
         "--enableTestCommands=true on the mongod."},
        {"--out",
         "[result, oneline]. Write results to a .results file after running. `result` formats "
         "each document on its own line; `oneline` puts the entire result set on one line. "
         "Overwrites existing .results files. Not available with --mode compare or when using "
         "-n or -r."},
        {"--populateAndExit",
         "Drop and reload collection data, then exit without running any tests. Implicitly "
         "applies --drop and --load. Accepts exactly one -t argument."},
        {"--uri",
         "MongoDB connection URI. Defaults to mongodb://localhost:27017 if not specified. "
         "Not used in --mode normalize."},
        // Short options second. Sorted in lexicographical order.
        {"-h", "Print this help string."},
        {"-n",
         "Run only the test at position <int> (0-indexed) in the immediately preceding -t file. "
         "For example, '-t foo.test -n 2' runs the third test in foo.test. "
         "Must immediately follow a -t argument. Incompatible with -r and --out."},
        {"-r",
         "Run only tests numbered <start> through <end> (inclusive) from the immediately "
         "preceding -t file. <start> must be <= <end>. Must immediately follow a -t argument. "
         "Incompatible with -n and --out."},
        {"-t",
         "Path to a .test file to run. Can be specified multiple times to run several test "
         "files in sequence."},
        {"-v (verbose)",
         "Print a summary of failing queries after an unsuccessful run. Requires --mode compare. "
         "Combine with --extractFeatures for richer per-failure diagnostics."},
        {"--override",
         "[queryShapeHash]. Override the test type for the run. Currently the only supported "
         "value is `queryShapeHash`, which runs explain on each query and asserts that the "
         "extracted query shape hash matches the expected value in a .queryShapeHash.results "
         "file."}};
    for (const auto& [key, val] : kHelpMap) {
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
    auto createAllIndices = true;
    auto dropOpt = false;
    auto extractFeatures = false;
    auto ignoreIndexFailures = false;
    auto loadOpt = false;
    auto mongoURIString = boost::optional<std::string>{};
    auto mode = ModeOption::Compare;  // Default.
    auto modeExplicitlySet = false;
    auto optimizationsOff = false;
    auto outOpt = WriteOutOptions::kNone;
    auto populateAndExit = false;
    auto verbose = false;
    auto diffStyle = DiffStyle::kWord;
    auto overrideOption = OverrideOption::None;
    for (auto argNum = size_t{1}; argNum < parsedArgs.size(); ++argNum) {
        // Same order as in the help menu.
        if (parsedArgs[argNum] == "--diff") {
            assertNextArgExists(parsedArgs, argNum, "--diff");
            diffStyle = stringToDiffStyle(parsedArgs[argNum + 1]);
            ++argNum;
        } else if (parsedArgs[argNum] == "--drop") {
            dropOpt = true;
        } else if (parsedArgs[argNum] == "--extractFeatures") {
            extractFeatures = true;
        } else if (parsedArgs[argNum] == "--ignore-index-failures") {
            ignoreIndexFailures = true;
        } else if (parsedArgs[argNum] == "--load") {
            loadOpt = true;
        } else if (parsedArgs[argNum] == "--minimal-index") {
            createAllIndices = false;
        } else if (parsedArgs[argNum] == "--mode") {
            assertNextArgExists(parsedArgs, argNum, "--mode");
            mode = stringToModeOption(parsedArgs[argNum + 1]);
            modeExplicitlySet = true;
            ++argNum;
        } else if (parsedArgs[argNum] == "--opt-off") {
            optimizationsOff = true;
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
                exitWithError(1,
                              "-n must immediately follow the -t it modifies and cannot be "
                              "combined with -r for the same test file");
            }
            runningPartialFile = true;
            assertNextArgExists(parsedArgs, argNum, "-n");
            testsToRun.back().startTest = std::stoull(parsedArgs[argNum + 1]);
            testsToRun.back().endTest = std::stoull(parsedArgs[argNum + 1]);
            ++argNum;
        } else if (parsedArgs[argNum] == "-r") {
            if (expectingNumAt != argNum) {
                exitWithError(1,
                              "-r must immediately follow the -t it modifies and cannot be "
                              "combined with -n for the same test file");
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
        } else if (parsedArgs[argNum] == "--override") {
            assertNextArgExists(parsedArgs, argNum, "--override");
            overrideOption = stringToOverrideOption(parsedArgs[argNum + 1]);
            ++argNum;
        } else {
            exitWithError(1, std::string{"Unexpected argument "} + parsedArgs[argNum]);
        }
    }

    // If --out is specified without an explicit --mode, default to run mode since writing out
    // results is only meaningful when running (not comparing).
    if (outOpt != WriteOutOptions::kNone && !modeExplicitlySet) {
        mode = ModeOption::Run;
    }

    if (!mongoURIString) {
        mongoURIString = "mongodb://localhost:27017";
        std::cout << "Using default URI of " << mongoURIString.get() << std::endl;
    }

    auto errorLogLevel = [&extractFeatures, &verbose]() -> ErrorLogLevel {
        if (extractFeatures) {
            return ErrorLogLevel::kExtractFeatures;
        } else if (verbose) {
            return ErrorLogLevel::kVerbose;
        } else {
            return ErrorLogLevel::kSimple;
        }
    }();

    // Validate some flag conditions.
    for (const auto& [condition, message] : std::map<bool, std::string>{
             {mode == ModeOption::Compare && outOpt != WriteOutOptions::kNone,
              "--mode compare and --out are incompatible."},
             // Cannot write out if only running part of a file.
             {runningPartialFile && outOpt != WriteOutOptions::kNone,
              "--out not supported with either -n or -r."},
             {mode == ModeOption::Normalize && (dropOpt || loadOpt),
              "--drop and --load are incompatible with --mode normalize."},
             {populateAndExit && testsToRun.size() != 1,
              "--populateAndExit must be specified with a single test file."},
             {testsToRun.empty(), "Make sure to provide QueryTester with a .test file."},
             {errorLogLevel == ErrorLogLevel::kVerbose && mode != ModeOption::Compare,
              "option -v must be specified with --mode compare."},
             {errorLogLevel == ErrorLogLevel::kExtractFeatures &&
                  (mode != ModeOption::Compare || !verbose),
              "--extractFeatures be specified with --mode compare and option -v (verbose)."}}) {
        if (condition) {
            exitWithError(1, message);
        }
    }

    // Validate test file paths and compare values.
    for (const auto& testSpec : testsToRun) {
        testSpec.validate(mode);
    }

    try {
        mongo::runGlobalInitializersOrDie(std::vector<std::string>(argv, argv + argc));
        auto serviceContextHolder = ServiceContext::make();
        setGlobalServiceContext(std::move(serviceContextHolder));
        return runTestProgram(std::move(testsToRun),
                              mongoURIString.get(),
                              dropOpt,
                              loadOpt,
                              createAllIndices,
                              ignoreIndexFailures,
                              outOpt,
                              mode,
                              optimizationsOff,
                              populateAndExit,
                              errorLogLevel,
                              diffStyle,
                              overrideOption);
    } catch (AssertionException& ex) {
        exitWithError(1, ex.reason());
    }
    MONGO_UNREACHABLE;
}
}  // namespace mongo::query_tester

int main(const int argc, const char** const argv) {
    return mongo::query_tester::queryTesterMain(argc, argv);
}
