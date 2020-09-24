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
#include "mongo/client/sdam/sdam_configuration_parameters_gen.h"
#include "mongo/client/sdam/server_description_builder.h"
#include "mongo/client/sdam/server_selector.h"
#include "mongo/client/sdam/topology_manager.h"
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

std::ostream& operator<<(std::ostream& os, const std::vector<HostAndPort>& input) {
    for (auto const& i : input) {
        os << i.toString() << " ";
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

    virtual bool errorIsExpected() {
        return false;
    }
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
        LOGV2(4333500, "### Running Test ###", "testFilePath"_attr = _testFilePath);

        ServerDescriptionPtr updatedServerDescription;
        if (_serverDescription) {
            updatedServerDescription = (*_serverDescription)->cloneWithRTT(Milliseconds(_newRtt));
        } else {
            // This is mocking the first ServerDescription being created during the handshake
            auto clockSource = std::make_unique<ClockSourceMock>();
            updatedServerDescription = std::make_shared<ServerDescription>(
                ServerDescription(clockSource.get(),
                                  IsMasterOutcome(HostAndPort("dummy"),
                                                  BSON("ok" << 1 << "setname"
                                                            << "replSet"
                                                            << "ismaster" << true),
                                                  IsMasterRTT(Milliseconds(_newRtt)))));
        }

        TestCaseResult result{{}, _testFilePath};
        validateNewAvgRtt(&result, updatedServerDescription);

        if (!result.Success()) {
            LOGV2(4333501, "Test failed", "testFilePath"_attr = _testFilePath);
        }

        return result;
    }

    const std::string& FilePath() const {
        return _testFilePath;
    }

private:
    void parseTest(fs::path testFilePath) {
        _testFilePath = testFilePath.string();
        LOGV2(4333503, "### Parsing Test ###", "testFilePath"_attr = testFilePath.string());
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
                                         .withAddress(HostAndPort("dummy"))
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
        try {
            parseTest(testFilePath);
        } catch (const DBException& ex) {
            _parseError = TestCaseResult{{std::make_pair("error while parsing", ex.toString())},
                                         testFilePath.string()};
        }
    }
    ~JsonServerSelectionTestCase() = default;

    TestCaseResult execute() {
        LOGV2(4333504, "### Running Test ###", "testFilePath"_attr = _testFilePath);
        if (_parseError)
            return *_parseError;

        TestCaseResult result{{}, _testFilePath};

        try {
            SdamServerSelector serverSelector(
                SdamConfiguration(std::vector<HostAndPort>{HostAndPort("foo:1234")}));
            auto selectedServers =
                serverSelector.selectServers(_topologyDescription, _readPreference);

            std::vector<std::string> selectedHosts;
            if (selectedServers) {
                std::transform((*selectedServers).begin(),
                               (*selectedServers).end(),
                               std::back_inserter(selectedHosts),
                               [](const ServerDescriptionPtr& server) {
                                   return server->getAddress().toString();
                               });
            }
            LOGV2(5017006, "Servers selected", "servers"_attr = selectedHosts);

            validateServersInLatencyWindow(&result, selectedServers);
        } catch (const DBException& ex) {
            auto errorDescription = std::make_pair("exception", ex.toString());
            result.errorDescriptions.push_back(errorDescription);
        }

        if (!result.Success() && !_errorExpected) {
            LOGV2(4333505, "Test failed", "testFilePath"_attr = _testFilePath);
        }

        return result;
    }

    const std::string& FilePath() const {
        return _testFilePath;
    }

    bool errorIsExpected() {
        return _errorExpected;
    }

private:
    void parseTest(fs::path testFilePath) {
        _testFilePath = testFilePath.string();
        LOGV2(4333507, "### Parsing Test ###", "testFilePath"_attr = testFilePath.string());
        {
            std::ifstream testFile(_testFilePath);
            std::ostringstream json;
            json << testFile.rdbuf();
            _jsonTest = fromjson(json.str());
        }

        // Do this first to save the state
        if (_jsonTest.hasField("error")) {
            LOGV2(5017004, "Expecting test case to generate an error.");
            _errorExpected = _jsonTest.getBoolField("error");
        }

        // The json tests pass in capitalized keywords for mode, but the server only accepts
        // lowercased keywords. Also, change the key "tags_set" to "tags".
        // This can throw for test cases that have invalid read preferences.
        auto readPrefObj = _jsonTest.getObjectField("read_preference");
        std::string mode = readPrefObj.getStringField("mode");
        mode[0] = std::tolower(mode[0]);
        auto tagSetsObj = readPrefObj["tag_sets"];
        auto tags = tagSetsObj ? BSONArray(readPrefObj["tag_sets"].Obj()) : BSONArray();


        auto topologyDescriptionObj = _jsonTest.getObjectField("topology_description");
        TopologyType initType =
            uassertStatusOK(parseTopologyType(topologyDescriptionObj.getStringField("type")));
        boost::optional<std::string> setName = boost::none;
        if (initType == TopologyType::kReplicaSetNoPrimary || initType == TopologyType::kSingle)
            setName = "replset";

        int maxStalenessSeconds = 0;
        if (readPrefObj.hasField("maxStalenessSeconds")) {
            maxStalenessSeconds = readPrefObj.getIntField("maxStalenessSeconds");
        }

        BSONObj readPref =
            BSON("mode" << mode << "tags" << tags << "maxStalenessSeconds" << maxStalenessSeconds);
        auto stalenessParseResult = ReadPreferenceSetting::fromInnerBSON(readPref);
        if (!_errorExpected &&
            stalenessParseResult.getStatus().code() == ErrorCodes::MaxStalenessOutOfRange &&
            (initType == TopologyType::kUnknown || initType == TopologyType::kSingle ||
             initType == TopologyType::kSharded)) {
            // Drivers tests expect no error for invalid maxStaleness values for Unknown, Single,
            // and Sharded topology types. The server code doesn't allow read preferences to be
            // created for invalid values for maxStaleness, so ignore it.
            readPref = BSON("mode" << mode << "tags" << tags << "maxStalenessSeconds" << 0);
            stalenessParseResult = ReadPreferenceSetting::fromInnerBSON(readPref);
        } else if (_errorExpected && maxStalenessSeconds == 0 &&
                   readPrefObj.hasField("maxStalenessSeconds")) {
            // Generate an error to pass test cases that set max staleness to zero since the server
            // interprets this as no maxStaleness is specified.
            uassert(ErrorCodes::MaxStalenessOutOfRange, "max staleness should be >= 0", false);
        }
        _readPreference = uassertStatusOK(stalenessParseResult);
        LOGV2(5017007, "Read Preference", "value"_attr = _readPreference);

        if (_jsonTest.hasField("heartbeatFrequencyMS")) {
            sdamHeartBeatFrequencyMs = _jsonTest.getIntField("heartbeatFrequencyMS");
            LOGV2(5017003, "Set heartbeatFrequencyMs", "value"_attr = sdamHeartBeatFrequencyMs);
        }

        std::vector<ServerDescriptionPtr> serverDescriptions;
        std::vector<HostAndPort> serverAddresses;
        const std::vector<BSONElement>& bsonServers = topologyDescriptionObj["servers"].Array();

        for (auto bsonServer : bsonServers) {
            auto server = bsonServer.Obj();

            int maxWireVersion = 9;
            if (server.hasField("maxWireVersion")) {
                maxWireVersion = server["maxWireVersion"].numberInt();
            }

            auto serverType = uassertStatusOK(parseServerType(server.getStringField("type")));
            auto serverDescription = ServerDescriptionBuilder()
                                         .withAddress(HostAndPort(server.getStringField("address")))
                                         .withType(serverType)
                                         .withRtt(Milliseconds(server["avg_rtt_ms"].numberInt()))
                                         .withMinWireVersion(5)
                                         .withMaxWireVersion(maxWireVersion);

            if (server.hasField("lastWrite")) {
                auto lastWriteDate =
                    server.getObjectField("lastWrite").getIntField("lastWriteDate");
                serverDescription.withLastWriteDate(Date_t::fromMillisSinceEpoch(lastWriteDate));
            }

            if (server.hasField("lastUpdateTime")) {
                auto lastUpdateTime = server.getIntField("lastUpdateTime");
                serverDescription.withLastUpdateTime(Date_t::fromMillisSinceEpoch(lastUpdateTime));
            }

            auto tagsObj = server.getObjectField("tags");
            const auto keys = tagsObj.getFieldNames<std::set<std::string>>();
            for (const auto& key : keys) {
                serverDescription.withTag(key, tagsObj.getStringField(key));
            }

            serverDescriptions.push_back(serverDescription.instance());
            LOGV2(5017002,
                  "Server Description",
                  "description"_attr = serverDescriptions.back()->toBson());
            serverAddresses.push_back(HostAndPort(server.getStringField("address")));
        }


        boost::optional<std::vector<HostAndPort>> seedList = boost::none;
        if (serverAddresses.size() > 0)
            seedList = serverAddresses;

        auto config = SdamConfiguration(seedList,
                                        initType,
                                        Milliseconds{sdamHeartBeatFrequencyMs},
                                        Milliseconds{sdamConnectTimeoutMs},
                                        Milliseconds{sdamLocalThreshholdMs},
                                        setName);
        _topologyDescription = std::make_shared<TopologyDescription>(config);

        if (_jsonTest.hasField("in_latency_window")) {
            const std::vector<BSONElement>& bsonLatencyWindow =
                _jsonTest["in_latency_window"].Array();
            for (const auto& serverDescription : serverDescriptions) {
                _topologyDescription->installServerDescription(serverDescription);

                for (auto bsonServer : bsonLatencyWindow) {
                    auto server = bsonServer.Obj();
                    if (serverDescription->getAddress() ==
                        HostAndPort(server.getStringField("address"))) {
                        _inLatencyWindow.push_back(serverDescription);
                    }
                }
            }
        }
    }

    void validateServersInLatencyWindow(
        TestCaseResult* result,
        boost::optional<std::vector<ServerDescriptionPtr>> selectedServers) {
        // Compare the server addresses of each server in the selectedServers and
        // _inLatencyWindow vectors. We do not need to compare the entire server description
        // because we only need to make sure that the correct server was chosen and are not
        // manipulating the ServerDescriptions at all.
        std::vector<HostAndPort> selectedHostAndPorts;
        std::vector<HostAndPort> expectedHostAndPorts;

        auto extractHost = [](const ServerDescriptionPtr& s) { return s->getAddress(); };
        if (selectedServers) {
            std::transform(selectedServers->begin(),
                           selectedServers->end(),
                           std::back_inserter(selectedHostAndPorts),
                           extractHost);
        }
        std::transform(_inLatencyWindow.begin(),
                       _inLatencyWindow.end(),
                       std::back_inserter(expectedHostAndPorts),
                       extractHost);

        std::sort(selectedHostAndPorts.begin(), selectedHostAndPorts.end());
        std::sort(expectedHostAndPorts.begin(), expectedHostAndPorts.end());
        if (!std::equal(selectedHostAndPorts.begin(),
                        selectedHostAndPorts.end(),
                        expectedHostAndPorts.begin(),
                        expectedHostAndPorts.end())) {
            std::stringstream errorMessage;
            errorMessage << "selected servers with addresses '" << selectedHostAndPorts
                         << "' server(s), but expected '" << expectedHostAndPorts
                         << "' to be selected.";
            auto errorDescription = std::make_pair("servers in latency window", errorMessage.str());
            result->errorDescriptions.push_back(errorDescription);
            return;
        }
    }

    std::string _testFilePath;
    BSONObj _jsonTest;
    TopologyDescriptionPtr _topologyDescription;
    ReadPreferenceSetting _readPreference;
    std::vector<ServerDescriptionPtr> _inLatencyWindow;
    boost::optional<TestCaseResult> _parseError;
    bool _errorExpected = false;
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
            int restoreHeartBeatFrequencyMs = sdamHeartBeatFrequencyMs;

            std::unique_ptr<JsonTestCase> testCase;
            try {
                testCase = [jsonTest]() -> std::unique_ptr<JsonTestCase> {
                    if (jsonTest.string().find("/rtt/") != std::string::npos ||
                        jsonTest.string().find("\\rtt\\") != std::string::npos) {
                        return std::make_unique<JsonRttTestCase>(jsonTest);
                    }
                    return std::make_unique<JsonServerSelectionTestCase>(jsonTest);
                }();
                LOGV2(
                    4333508, "### Executing Test ###", "testFilePath"_attr = testCase->FilePath());
                auto executionResult = testCase->execute();
                if (testCase->errorIsExpected() && executionResult.Success()) {
                    auto errorDescription =
                        std::make_pair("failure expected", "Expected test to fail, but it didn't");
                    executionResult.errorDescriptions.push_back(errorDescription);
                } else if (testCase->errorIsExpected() && !executionResult.Success()) {
                    // clear the errors, so that it's treated as a success
                    executionResult.errorDescriptions.clear();
                }
                results.push_back(executionResult);
            } catch (const DBException& ex) {
                if (!testCase || !testCase->errorIsExpected()) {
                    std::stringstream error;
                    error << "Exception while executing " << jsonTest.string() << ": "
                          << ex.toString();
                    std::string errorStr = error.str();
                    results.push_back(JsonTestCase::TestCaseResult{
                        {std::make_pair("exception", errorStr)}, jsonTest.string()});
                    std::cerr << errorStr;
                }
            }

            // use default value of sdamHeartBeatFrequencyMs unless the test explicitly sets it.
            sdamHeartBeatFrequencyMs = restoreHeartBeatFrequencyMs;
        }
        return results;
    }

    std::string collectErrorStr(const std::vector<JsonTestCase::TestErrors>& errors) {
        std::string result;
        for (size_t i = 0; i < errors.size(); ++i) {
            auto error = errors[i];
            result = result + error.first + " - " + error.second;
            if (i != errors.size() - 1) {
                result += "; ";
            }
        }
        return result;
    }


    int report(std::vector<JsonTestCase::TestCaseResult> results) {
        int numTestCases = results.size();
        int numSuccess = 0;
        int numFailed = 0;

        if (std::any_of(
                results.begin(), results.end(), [](const JsonTestCase::TestCaseResult& result) {
                    return !result.Success();
                })) {
            LOGV2(4333509, "### Failed Test Results ###");
        }

        for (const auto& result : results) {
            auto file = result.file;
            if (result.Success()) {
                ++numSuccess;
            } else {
                LOGV2(4333510,
                      "### Failed Test File ###",
                      "testFilePath"_attr = file,
                      "errors"_attr = collectErrorStr(result.errorDescriptions));
                ++numFailed;
            }
        }
        LOGV2(4333513,
              "Results summary",
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
                            "Test skipped due to filter configuration",
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

    ::mongo::logv2::LogManager::global().getGlobalSettings().setMinimumLoggedSeverity(
        MONGO_LOGV2_DEFAULT_COMPONENT, ::mongo::logv2::LogSeverity::Debug(args.Verbose()));
    args.LogParams();

    ServerSelectionJsonTestRunner testRunner(args.SourceDirectory(), args.TestFilters());
    return testRunner.report(testRunner.runTests());
}
