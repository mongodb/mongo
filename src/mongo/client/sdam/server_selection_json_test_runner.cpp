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
#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

#include <fstream>
#include <iostream>
#include <memory>

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/format.hpp>
#include <boost/optional/optional_io.hpp>

#include "mongo/bson/json.h"
#include "mongo/client/sdam/json_test_arg_parser.h"
#include "mongo/client/sdam/server_description_builder.h"
#include "mongo/client/sdam/server_selector.h"
#include "mongo/client/sdam/topology_manager.h"
#include "mongo/logger/logger.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/options_parser/environment.h"
#include "mongo/util/options_parser/option_section.h"
#include "mongo/util/options_parser/options_parser.h"

/**
 * This program runs the Server Discover and Monitoring JSON test files located in
 * the src/monogo/client/sdam/json_tests/server_selection_tests sub-directory.
 *
 * The process return code conforms to the UNIX idiom of 0 to indicate success and non-zero to
 * indicate failure. In the case of test failure, the process will return the number of test cases
 * that failed.
 *
 * Example invocation to run all tests:
 *  sdam_json_test --source-dir src/monogo/client/sdam/json_tests/server_selection_tests
 *
 * Example invocation to run a single test:
 *  sdam_json_test --source-dir src/monogo/client/sdam/json_tests/server_selection_tests --filter
 * Nearest_multiple
 */

namespace fs = boost::filesystem;
namespace moe = mongo::optionenvironment;
using namespace mongo::sdam;

namespace mongo::sdam {

std::string emphasize(const std::string text) {
    std::stringstream output;
    const auto border = std::string(3, '#');
    output << border << " " << text << " " << border << std::endl;
    return output.str();
}

std::ostream& operator<<(std::ostream& os, const std::vector<std::string>& input) {
    for (auto const& i : input) {
        os << i << " ";
    }
    return os;
}

class JsonTestCase {
public:
    virtual ~JsonTestCase() = default;

    // pair of error subject & error description
    using TestErrors = std::pair<std::string, std::string>;
    struct TestCaseResult {
        std::vector<TestErrors> errorDescriptions;
        std::string file;

        bool Success() const {
            return errorDescriptions.size() == 0;
        }
    };

    virtual TestCaseResult execute() = 0;

    virtual const std::string& FilePath() const = 0;
};

/**
 * This class is responsible for parsing and executing a single json test file in the rtt directory.
 */
class JsonRttTestCase : public JsonTestCase {
public:
    JsonRttTestCase(fs::path testFilePath) {
        parseTest(testFilePath);
    }

    ~JsonRttTestCase() = default;

    TestCaseResult execute() {
        LOGV2(
            4333500, "{testFilePath}", "testFilePath"_attr = emphasize("Running " + _testFilePath));

        ServerDescriptionPtr updatedServerDescription;
        if (_serverDescription) {
            updatedServerDescription = (*_serverDescription)->cloneWithRTT(Milliseconds(_newRtt));
        } else {
            // This is mocking the first ServerDescription being created during the handshake
            auto clockSource = std::make_unique<ClockSourceMock>();
            updatedServerDescription = std::make_shared<ServerDescription>(
                ServerDescription(clockSource.get(),
                                  IsMasterOutcome(ServerAddress("dummy"),
                                                  BSON("ok" << 1 << "setname"
                                                            << "replSet"
                                                            << "ismaster" << true),
                                                  IsMasterRTT(Milliseconds(_newRtt)))));
        }

        TestCaseResult result{{}, _testFilePath};
        validateNewAvgRtt(&result, updatedServerDescription);

        if (!result.Success()) {
            LOGV2(4333501, "Test {testFilePath} failed.", "testFilePath"_attr = _testFilePath);
        }

        return result;
    }

    const std::string& FilePath() const {
        return _testFilePath;
    }

private:
    void parseTest(fs::path testFilePath) {
        _testFilePath = testFilePath.string();
        LOGV2(4333502, "");
        LOGV2(4333503,
              "{testFilePath}",
              "testFilePath"_attr = emphasize("Parsing " + testFilePath.string()));
        {
            std::ifstream testFile(_testFilePath);
            std::ostringstream json;
            json << testFile.rdbuf();
            _jsonTest = fromjson(json.str());
        }

        // Only create the initial server description if the original avg rtt is not "NULL". If it
        // is, the test case is meant to mimic creating the first ServerDescription which we will do
        // above.
        std::string origRttAsString = _jsonTest.getStringField("avg_rtt_ms");
        if (origRttAsString.compare("NULL") != 0) {
            auto serverDescription = ServerDescriptionBuilder()
                                         .withAddress("dummy")
                                         .withType(ServerType::kRSPrimary)
                                         .instance();
            auto origAvgRtt = Milliseconds(_jsonTest["avg_rtt_ms"].numberInt());

            _serverDescription = serverDescription->cloneWithRTT(origAvgRtt);
        }

        _newRtt = _jsonTest["new_rtt_ms"].numberInt();
        _newAvgRtt = _jsonTest["new_avg_rtt"].numberInt();
    }

    void validateNewAvgRtt(TestCaseResult* result, ServerDescriptionPtr newServerDescription) {
        if (!newServerDescription->getRtt()) {
            std::stringstream errorMessage;
            errorMessage << "new server description does not have an RTT value but expected '"
                         << Milliseconds(_newAvgRtt) << "'ms";
            auto errorDescription = std::make_pair("RTT", errorMessage.str());
            result->errorDescriptions.push_back(errorDescription);
            return;
        }

        auto newAvgRtt = duration_cast<Milliseconds>(newServerDescription->getRtt().get());
        if (newAvgRtt.compare(duration_cast<Milliseconds>(Milliseconds(_newAvgRtt))) != 0) {
            std::stringstream errorMessage;
            errorMessage << "new average RTT is incorrect, got '" << newAvgRtt
                         << "'ms but expected '" << Milliseconds(_newAvgRtt) << "'ms";
            auto errorDescription = std::make_pair("RTT", errorMessage.str());
            result->errorDescriptions.push_back(errorDescription);
        }
    }

    std::string _testFilePath;
    BSONObj _jsonTest;
    boost::optional<ServerDescriptionPtr> _serverDescription;
    int _newRtt;
    int _newAvgRtt;
};

/**
 * This class is responsible for parsing and executing a single json test file in the
 * server_selection directory.
 */
class JsonServerSelectionTestCase : public JsonTestCase {
public:
    JsonServerSelectionTestCase(fs::path testFilePath) {
        parseTest(testFilePath);
    }
    ~JsonServerSelectionTestCase() = default;

    TestCaseResult execute() {
        LOGV2(
            4333504, "{testFilePath}", "testFilePath"_attr = emphasize("Running " + _testFilePath));

        SdamServerSelector serverSelector(
            sdam::ServerSelectionConfiguration::defaultConfiguration());
        auto selectedServers = serverSelector.selectServers(_topologyDescription, _readPreference);

        TestCaseResult result{{}, _testFilePath};
        validateServersInLatencyWindow(&result, selectedServers);

        if (!result.Success()) {
            LOGV2(4333505, "Test {testFilePath} failed.", "testFilePath"_attr = _testFilePath);
        }

        return result;
    }

    const std::string& FilePath() const {
        return _testFilePath;
    }

private:
    void parseTest(fs::path testFilePath) {
        _testFilePath = testFilePath.string();
        LOGV2(4333506, "");
        LOGV2(4333507,
              "{testFilePath}",
              "testFilePath"_attr = emphasize("Parsing " + testFilePath.string()));
        {
            std::ifstream testFile(_testFilePath);
            std::ostringstream json;
            json << testFile.rdbuf();
            _jsonTest = fromjson(json.str());
        }

        // The json tests pass in capitalized keywords for mode, but the server only accepts
        // lowercased keywords. Also, change the key "tags_set" to "tags".
        auto readPrefObj = _jsonTest.getObjectField("read_preference");
        std::string mode = readPrefObj.getStringField("mode");
        mode[0] = std::tolower(mode[0]);
        auto tagSetsObj = readPrefObj["tag_sets"];
        auto tags = tagSetsObj ? BSONArray(readPrefObj["tag_sets"].Obj()) : BSONArray();

        BSONObj readPref = BSON("mode" << mode << "tags" << tags);
        _readPreference = uassertStatusOK(ReadPreferenceSetting::fromInnerBSON(readPref));

        // Parse the TopologyDescription and inLatencyWindow objects
        auto topologyDescriptionObj = _jsonTest.getObjectField("topology_description");

        std::vector<ServerDescriptionPtr> serverDescriptions;
        std::vector<ServerAddress> serverAddresses;
        const std::vector<BSONElement>& bsonServers = topologyDescriptionObj["servers"].Array();
        for (auto bsonServer : bsonServers) {
            auto server = bsonServer.Obj();

            auto serverType = uassertStatusOK(parseServerType(server.getStringField("type")));
            auto serverDescription = ServerDescriptionBuilder()
                                         .withAddress(server.getStringField("address"))
                                         .withType(serverType)
                                         .withRtt(Milliseconds(server["avg_rtt_ms"].numberInt()))
                                         .withMinWireVersion(8)
                                         .withMaxWireVersion(9);

            auto tagsObj = server.getObjectField("tags");
            const auto keys = tagsObj.getFieldNames<std::set<std::string>>();
            for (const auto key : keys) {
                serverDescription.withTag(key, tagsObj.getStringField(key));
            }

            serverDescriptions.push_back(serverDescription.instance());
            serverAddresses.push_back(server.getStringField("address"));
        }

        TopologyType initType =
            uassertStatusOK(parseTopologyType(topologyDescriptionObj.getStringField("type")));
        boost::optional<std::string> setName = boost::none;
        if (initType == TopologyType::kReplicaSetNoPrimary || initType == TopologyType::kSingle)
            setName = "replset";

        boost::optional<std::vector<ServerAddress>> seedList = boost::none;
        if (serverAddresses.size() > 0)
            seedList = serverAddresses;

        auto config = SdamConfiguration(
            seedList, initType, SdamConfiguration::kDefaultHeartbeatFrequencyMs, setName);
        _topologyDescription = std::make_shared<TopologyDescription>(config);

        const std::vector<BSONElement>& bsonLatencyWindow = _jsonTest["in_latency_window"].Array();
        for (const auto& serverDescription : serverDescriptions) {
            _topologyDescription->installServerDescription(serverDescription);

            for (auto bsonServer : bsonLatencyWindow) {
                auto server = bsonServer.Obj();
                if (serverDescription->getAddress() ==
                    ServerAddress(server.getStringField("address"))) {
                    _inLatencyWindow.push_back(serverDescription);
                }
            }
        }
    }

    void validateServersInLatencyWindow(
        TestCaseResult* result,
        boost::optional<std::vector<ServerDescriptionPtr>> selectedServers) {
        if (!selectedServers && _inLatencyWindow.size() > 0) {
            std::stringstream errorMessage;
            errorMessage << "did not select any servers, but expected '" << _inLatencyWindow.size()
                         << "' to be selected.";
            auto errorDescription = std::make_pair("servers in latency window", errorMessage.str());
            result->errorDescriptions.push_back(errorDescription);
        } else if (selectedServers && selectedServers->size() != _inLatencyWindow.size()) {
            std::stringstream errorMessage;
            errorMessage << "selected '" << selectedServers->size() << "' server(s), but expected '"
                         << _inLatencyWindow.size() << "' to be selected.";
            auto errorDescription = std::make_pair("servers in latency window", errorMessage.str());
            result->errorDescriptions.push_back(errorDescription);
        } else {
            // Compare the server addresses of each server in the selectedServers and
            // _inLatencyWindow vectors. We do not need to compare the entire server description
            // because we only need to make sure that the correct server was chosen and are not
            // manipulating the ServerDescriptions at all.
            std::vector<std::string> selectedServerAddresses;
            std::vector<std::string> expectedServerAddresses;

            auto selectedServersIt = selectedServers->begin();
            for (auto expectedServersIt = _inLatencyWindow.begin();
                 expectedServersIt != _inLatencyWindow.end();
                 ++expectedServersIt) {
                selectedServerAddresses.push_back((*selectedServersIt)->getAddress());
                expectedServerAddresses.push_back((*expectedServersIt)->getAddress());

                selectedServersIt++;
            }

            std::sort(selectedServerAddresses.begin(), selectedServerAddresses.end());
            std::sort(expectedServerAddresses.begin(), expectedServerAddresses.end());
            if (!std::equal(selectedServerAddresses.begin(),
                            selectedServerAddresses.end(),
                            expectedServerAddresses.begin())) {
                std::stringstream errorMessage;
                errorMessage << "selected servers with addresses '" << selectedServerAddresses
                             << "' server(s), but expected '" << expectedServerAddresses
                             << "' to be selected.";
                auto errorDescription =
                    std::make_pair("servers in latency window", errorMessage.str());
                result->errorDescriptions.push_back(errorDescription);
                return;
            }
        }
    }

    std::string _testFilePath;
    BSONObj _jsonTest;
    TopologyDescriptionPtr _topologyDescription;
    ReadPreferenceSetting _readPreference;
    std::vector<ServerDescriptionPtr> _inLatencyWindow;
};

/**
 * This class runs (potentially) multiple json tests and reports their results.
 */
class ServerSelectionJsonTestRunner {
public:
    ServerSelectionJsonTestRunner(std::string testDirectory, std::vector<std::string> testFilters)
        : _testFiles(scanTestFiles(testDirectory, testFilters)) {}

    std::vector<JsonTestCase::TestCaseResult> runTests() {
        std::vector<JsonTestCase::TestCaseResult> results;
        const auto testFiles = getTestFiles();
        for (auto jsonTest : testFiles) {
            auto testCase = [jsonTest]() -> std::unique_ptr<JsonTestCase> {
                if (jsonTest.string().find("/rtt/")) {
                    return std::make_unique<JsonRttTestCase>(jsonTest);
                }
                return std::make_unique<JsonServerSelectionTestCase>(jsonTest);
            }();

            try {
                LOGV2(4333508,
                      "{testFilePath}",
                      "testFilePath"_attr = emphasize("Executing " + testCase->FilePath()));
                results.push_back(testCase->execute());
            } catch (const DBException& ex) {
                std::stringstream error;
                error << "Exception while executing " << jsonTest.string() << ": " << ex.toString();
                std::string errorStr = error.str();
                results.push_back(JsonTestCase::TestCaseResult{
                    {std::make_pair("exception", errorStr)}, jsonTest.string()});
                std::cerr << errorStr;
            }
        }
        return results;
    }

    int report(std::vector<JsonTestCase::TestCaseResult> results) {
        int numTestCases = results.size();
        int numSuccess = 0;
        int numFailed = 0;

        if (std::any_of(
                results.begin(), results.end(), [](const JsonTestCase::TestCaseResult& result) {
                    return !result.Success();
                })) {
            LOGV2(4333509,
                  "{Failed_Test_Results}",
                  "Failed_Test_Results"_attr = emphasize("Failed Test Results"));
        }

        for (const auto result : results) {
            auto file = result.file;
            if (result.Success()) {
                ++numSuccess;
            } else {
                LOGV2(4333510, "{testFilePath}", "testFilePath"_attr = emphasize(file));
                LOGV2(4333511, "error in file: {file}", "file"_attr = file);
                ++numFailed;
                LOGV2(4333512, "");
            }
        }
        LOGV2(4333513,
              "{numTestCases} test cases; {numSuccess} success; {numFailed} failed.",
              "numTestCases"_attr = numTestCases,
              "numSuccess"_attr = numSuccess,
              "numFailed"_attr = numFailed);

        return numFailed;
    }

    const std::vector<fs::path>& getTestFiles() const {
        return _testFiles;
    }

private:
    std::vector<fs::path> scanTestFiles(std::string testDirectory,
                                        std::vector<std::string> filters) {
        std::vector<fs::path> results;
        for (const auto& entry : fs::recursive_directory_iterator(testDirectory)) {
            if (!fs::is_directory(entry) && matchesFilter(entry, filters)) {
                results.push_back(entry.path());
            }
        }
        return results;
    }

    bool matchesFilter(const fs::directory_entry& entry, std::vector<std::string> filters) {
        const auto filePath = entry.path();
        if (filePath.extension() != ".json") {
            return false;
        }

        if (filters.size() == 0) {
            return true;
        }

        for (const auto& filter : filters) {
            if (filePath.string().find(filter) != std::string::npos) {
                return true;
            } else {
                LOGV2_DEBUG(4333514,
                            2,
                            "'{filePath}' skipped due to filter configuration.",
                            "filePath"_attr = filePath.string());
            }
        }

        return false;
    }

    std::vector<fs::path> _testFiles;
};
};  // namespace mongo::sdam

int main(int argc, char* argv[]) {
    ArgParser args(argc, argv);

    ::mongo::logger::globalLogDomain()->setMinimumLoggedSeverity(
        ::mongo::logv2::LogSeverity::Debug(args.Verbose()));
    args.LogParams();

    ServerSelectionJsonTestRunner testRunner(args.SourceDirectory(), args.TestFilters());
    return testRunner.report(testRunner.runTests());
}
