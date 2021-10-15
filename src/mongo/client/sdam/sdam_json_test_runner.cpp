/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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
#include "mongo/client/mongo_uri.h"
#include "mongo/client/sdam/json_test_arg_parser.h"
#include "mongo/client/sdam/sdam_configuration_parameters_gen.h"
#include "mongo/client/sdam/topology_manager.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/options_parser/environment.h"
#include "mongo/util/options_parser/option_section.h"
#include "mongo/util/options_parser/options_parser.h"

/**
 * This program runs the Server Discover and Monitoring JSON test files located in
 * the src/monogo/client/sdam/json_tests/sdam_tests sub-directory.
 *
 * The process return code conforms to the UNIX idiom of 0 to indicate success and non-zero to
 * indicate failure. In the case of test failure, the process will return the number of test cases
 * that failed.
 *
 * Example invocation to run all tests:
 *  sdam_json_test --source-dir src/monogo/client/sdam/json_tests/sdam_tests
 *
 * Example invocation to run a single test:
 *  sdam_json_test --source-dir src/monogo/client/sdam/json_tests/sdam_tests --filter
 * normalize_uri_case
 */

namespace fs = boost::filesystem;
namespace moe = mongo::optionenvironment;
using namespace mongo::sdam;

namespace mongo::sdam {

/**
 * This class is responsible for parsing and executing a single 'phase' of the json test
 */
class TestCasePhase {
public:
    TestCasePhase(int phaseNum, MongoURI uri, BSONObj phase) : _testUri(uri), _phaseNum(phaseNum) {
        auto bsonResponses = phase.getField("responses").Array();
        for (auto& response : bsonResponses) {
            const auto pair = response.Array();
            const auto address = HostAndPort(pair[0].String());
            const auto bsonIsMaster = pair[1].Obj();

            if (bsonIsMaster.nFields() == 0) {
                _isMasterResponses.push_back(HelloOutcome(address, BSONObj(), "network error"));
            } else {
                _isMasterResponses.push_back(
                    HelloOutcome(address, bsonIsMaster, duration_cast<HelloRTT>(kLatency)));
            }
        }
        _topologyOutcome = phase["outcome"].Obj();
    }

    // pair of error subject & error description
    using TestPhaseError = std::pair<std::string, std::string>;
    struct PhaseResult {
        std::vector<TestPhaseError> errorDescriptions;
        int phaseNumber;

        bool Success() const {
            return errorDescriptions.size() == 0;
        }
    };
    using PhaseResultPtr = PhaseResult*;

    PhaseResult execute(TopologyManager& topology) const {
        PhaseResult testResult{{}, _phaseNum};

        for (auto response : _isMasterResponses) {
            auto descriptionStr =
                (response.getResponse()) ? response.getResponse()->toString() : "[ Network Error ]";
            LOGV2(20202,
                  "Sending server description",
                  "server"_attr = response.getServer(),
                  "description"_attr = descriptionStr);
            topology.onServerDescription(response);
        }

        LOGV2(20203,
              "TopologyDescription after phase",
              "phaseNumber"_attr = _phaseNum,
              "topologyDescription"_attr = topology.getTopologyDescription()->toString());

        validateServers(
            &testResult, topology.getTopologyDescription(), _topologyOutcome["servers"].Obj());
        validateTopologyDescription(
            &testResult, topology.getTopologyDescription(), _topologyOutcome);

        return testResult;
    }

    int getPhaseNum() const {
        return _phaseNum;
    }

private:
    template <typename T, typename U>
    std::string errorMessageNotEqual(T expected, U actual) const {
        std::stringstream errorMessage;
        errorMessage << "expected '" << actual << "' to equal '" << expected << "'";
        return errorMessage.str();
    }

    std::string serverDescriptionFieldName(const ServerDescriptionPtr serverDescription,
                                           std::string field) const {
        std::stringstream name;
        name << "(" << serverDescription->getAddress() << ") " << field;
        return name.str();
    }

    std::string topologyDescriptionFieldName(std::string field) const {
        std::stringstream name;
        name << "(topologyDescription) " << field;
        return name.str();
    }

    template <typename EVO, typename AV>
    void doValidateServerField(const PhaseResultPtr result,
                               const ServerDescriptionPtr serverDescription,
                               const std::string fieldName,
                               EVO expectedValueObtainer,
                               const AV& actualValue) const {
        const auto expectedValue = expectedValueObtainer();
        if (expectedValue != actualValue) {
            auto errorDescription =
                std::make_pair(serverDescriptionFieldName(serverDescription, fieldName),
                               errorMessageNotEqual(expectedValue, actualValue));
            result->errorDescriptions.push_back(errorDescription);
        }
    }

    void validateServerField(const PhaseResultPtr result,
                             const ServerDescriptionPtr& serverDescription,
                             const BSONElement& expectedField) const {
        const auto serverAddress = serverDescription->getAddress();

        std::string fieldName = expectedField.fieldName();
        if (fieldName == "type") {
            doValidateServerField(result,
                                  serverDescription,
                                  fieldName,
                                  [&]() {
                                      auto status = parseServerType(expectedField.String());
                                      if (!status.isOK()) {
                                          auto errorDescription = std::make_pair(
                                              serverDescriptionFieldName(serverDescription, "type"),
                                              status.getStatus().toString());
                                          result->errorDescriptions.push_back(errorDescription);

                                          // return the actual value since we already have reported
                                          // an error about the parsed server type from the json
                                          // file.
                                          return serverDescription->getType();
                                      }
                                      return status.getValue();
                                  },
                                  serverDescription->getType());

        } else if (fieldName == "setName") {
            doValidateServerField(result,
                                  serverDescription,
                                  fieldName,
                                  [&]() {
                                      boost::optional<std::string> result;
                                      if (expectedField.type() != BSONType::jstNULL) {
                                          result = expectedField.String();
                                      }
                                      return result;
                                  },
                                  serverDescription->getSetName());

        } else if (fieldName == "setVersion") {
            doValidateServerField(result,
                                  serverDescription,
                                  fieldName,
                                  [&]() {
                                      boost::optional<int> result;
                                      if (expectedField.type() != BSONType::jstNULL) {
                                          result = expectedField.numberInt();
                                      }
                                      return result;
                                  },
                                  serverDescription->getElectionIdSetVersionPair().setVersion);

        } else if (fieldName == "electionId") {
            doValidateServerField(result,
                                  serverDescription,
                                  fieldName,
                                  [&]() {
                                      boost::optional<OID> result;
                                      if (expectedField.type() != BSONType::jstNULL) {
                                          result = expectedField.OID();
                                      }
                                      return result;
                                  },
                                  serverDescription->getElectionIdSetVersionPair().electionId);

        } else if (fieldName == "logicalSessionTimeoutMinutes") {
            doValidateServerField(result,
                                  serverDescription,
                                  fieldName,
                                  [&]() {
                                      boost::optional<int> result;
                                      if (expectedField.type() != BSONType::jstNULL) {
                                          result = expectedField.numberInt();
                                      }
                                      return result;
                                  },
                                  serverDescription->getLogicalSessionTimeoutMinutes());

        } else if (fieldName == "minWireVersion") {
            doValidateServerField(result,
                                  serverDescription,
                                  fieldName,
                                  [&]() { return expectedField.numberInt(); },
                                  serverDescription->getMinWireVersion());

        } else if (fieldName == "maxWireVersion") {
            doValidateServerField(result,
                                  serverDescription,
                                  fieldName,
                                  [&]() { return expectedField.numberInt(); },
                                  serverDescription->getMaxWireVersion());

        } else {
            MONGO_UNREACHABLE;
        }
    }

    void validateServers(PhaseResultPtr result,
                         const TopologyDescriptionPtr topologyDescription,
                         const BSONObj bsonServers) const {
        auto actualNumServers = topologyDescription->getServers().size();
        auto expectedNumServers =
            bsonServers.getFieldNames<stdx::unordered_set<std::string>>().size();

        if (actualNumServers != expectedNumServers) {
            std::stringstream errorMessage;
            errorMessage << "expected " << expectedNumServers
                         << " server(s) in topology description. actual was " << actualNumServers
                         << ": ";
            for (const auto& server : topologyDescription->getServers()) {
                errorMessage << server->getAddress() << ", ";
            }
            result->errorDescriptions.push_back(std::make_pair("servers", errorMessage.str()));
        }

        for (const BSONElement& bsonExpectedServer : bsonServers) {
            const auto& serverAddress = HostAndPort(bsonExpectedServer.fieldName());
            const auto& expectedServerDescriptionFields = bsonExpectedServer.Obj();

            const auto& serverDescription = topologyDescription->findServerByAddress(serverAddress);
            if (serverDescription) {
                for (const BSONElement& field : expectedServerDescriptionFields) {
                    validateServerField(result, *serverDescription, field);
                }
            } else {
                std::stringstream errorMessage;
                errorMessage << "could not find server '" << serverAddress
                             << "' in topology description.";
                auto errorDescription = std::make_pair("servers", errorMessage.str());
                result->errorDescriptions.push_back(errorDescription);
            }
        }
    }

    template <typename EVO, typename AV>
    void doValidateTopologyDescriptionField(const PhaseResultPtr result,
                                            const std::string fieldName,
                                            EVO expectedValueObtainer,
                                            const AV& actualValue) const {
        auto expectedValue = expectedValueObtainer();
        if (expectedValue != actualValue) {
            auto errorDescription =
                std::make_pair(topologyDescriptionFieldName(fieldName),
                               errorMessageNotEqual(expectedValue, actualValue));
            result->errorDescriptions.push_back(errorDescription);
        }
    }

    void validateTopologyDescription(PhaseResultPtr result,
                                     const TopologyDescriptionPtr topologyDescription,
                                     const BSONObj bsonTopologyDescription) const {
        {
            constexpr auto fieldName = "topologyType";
            doValidateTopologyDescriptionField(
                result,
                fieldName,
                [&]() { return bsonTopologyDescription[fieldName].String(); },
                toString(topologyDescription->getType()));
        }

        {
            constexpr auto fieldName = "setName";
            doValidateTopologyDescriptionField(result,
                                               fieldName,
                                               [&]() {
                                                   boost::optional<std::string> ret;
                                                   auto bsonField =
                                                       bsonTopologyDescription[fieldName];
                                                   if (!bsonField.isNull()) {
                                                       ret = bsonField.String();
                                                   }
                                                   return ret;
                                               },
                                               topologyDescription->getSetName());
        }

        {
            constexpr auto fieldName = "logicalSessionTimeoutMinutes";
            doValidateTopologyDescriptionField(
                result,
                fieldName,
                [&]() {
                    boost::optional<int> ret;
                    auto bsonField = bsonTopologyDescription[fieldName];
                    if (!bsonField.isNull()) {
                        ret = bsonField.numberInt();
                    }
                    return ret;
                },
                topologyDescription->getLogicalSessionTimeoutMinutes());
        }

        {
            constexpr auto fieldName = "maxSetVersion";
            if (bsonTopologyDescription.hasField(fieldName)) {
                doValidateTopologyDescriptionField(
                    result,
                    fieldName,
                    [&]() {
                        boost::optional<int> ret;
                        auto bsonField = bsonTopologyDescription[fieldName];
                        if (!bsonField.isNull()) {
                            ret = bsonField.numberInt();
                        }
                        return ret;
                    },
                    topologyDescription->getMaxElectionIdSetVersionPair().setVersion);
            }
        }

        {
            constexpr auto fieldName = "maxElectionId";
            if (bsonTopologyDescription.hasField(fieldName)) {
                doValidateTopologyDescriptionField(
                    result,
                    fieldName,
                    [&]() {
                        boost::optional<OID> ret;
                        auto bsonField = bsonTopologyDescription[fieldName];
                        if (!bsonField.isNull()) {
                            ret = bsonField.OID();
                        }
                        return ret;
                    },
                    topologyDescription->getMaxElectionIdSetVersionPair().electionId);
            }
        }

        {
            constexpr auto fieldName = "compatible";
            if (bsonTopologyDescription.hasField(fieldName)) {
                doValidateTopologyDescriptionField(
                    result,
                    fieldName,
                    [&]() { return bsonTopologyDescription[fieldName].Bool(); },
                    topologyDescription->isWireVersionCompatible());
            }
        }
    }

    // the json tests don't actually use this value.
    constexpr static auto kLatency = mongo::Milliseconds(100);

    MongoURI _testUri;
    int _phaseNum;
    std::vector<HelloOutcome> _isMasterResponses;
    BSONObj _topologyOutcome;
};

/**
 * This class is responsible for parsing and executing a single json test file.
 */
class JsonTestCase {
public:
    JsonTestCase(fs::path testFilePath) {
        parseTest(testFilePath);
    }

    struct TestCaseResult {
        std::vector<TestCasePhase::PhaseResult> phaseResults;
        std::string file;
        std::string name;

        bool Success() const {
            return std::all_of(
                phaseResults.begin(),
                phaseResults.end(),
                [](const TestCasePhase::PhaseResult& result) { return result.Success(); });
        }
    };

    TestCaseResult execute() {
        auto config =
            std::make_unique<SdamConfiguration>(getSeedList(),
                                                _initialType,
                                                Milliseconds{kHeartBeatFrequencyMsDefault},
                                                Milliseconds{kConnectTimeoutMsDefault},
                                                Milliseconds{kLocalThresholdMsDefault},
                                                _replicaSetName);

        auto clockSource = std::make_unique<ClockSourceMock>();
        TopologyManagerImpl topology(*config, clockSource.get());

        TestCaseResult result{{}, _testFilePath, _testName};

        for (const auto& testPhase : _testPhases) {
            LOGV2(20204, "### Phase Number ###", "phase"_attr = testPhase.getPhaseNum());
            auto phaseResult = testPhase.execute(topology);
            result.phaseResults.push_back(phaseResult);
            if (!result.Success()) {
                LOGV2(20205, "Phase failed", "phase"_attr = phaseResult.phaseNumber);
                break;
            }
        }

        return result;
    }

    const std::string& Name() const {
        return _testName;
    }

private:
    void parseTest(fs::path testFilePath) {
        _testFilePath = testFilePath.string();
        LOGV2(20207, "### Parsing Test File ###", "testFilePath"_attr = testFilePath.string());
        {
            std::ifstream testFile(_testFilePath);
            std::ostringstream json;
            json << testFile.rdbuf();
            _jsonTest = fromjson(json.str());
        }

        _testName = _jsonTest.getStringField("description");
        _testUri = uassertStatusOK(mongo::MongoURI::parse(_jsonTest["uri"].String()));

        _replicaSetName = _testUri.getOption("replicaSet");
        if (!_replicaSetName) {
            if (_testUri.getServers().size() == 1) {
                _initialType = TopologyType::kSingle;
            } else {
                // We can technically choose either kUnknown or kSharded and be compliant,
                // but it seems that some of the json tests assume kUnknown as the initial state.
                // see: json_tests/sdam_tests/sharded/normalize_uri_case.json
                _initialType = TopologyType::kUnknown;
            }
        } else {
            _initialType = TopologyType::kReplicaSetNoPrimary;
        }

        int phase = 0;
        const std::vector<BSONElement>& bsonPhases = _jsonTest["phases"].Array();
        for (auto bsonPhase : bsonPhases) {
            _testPhases.push_back(TestCasePhase(phase++, _testUri, bsonPhase.Obj()));
        }
    }

    std::vector<HostAndPort> getSeedList() {
        std::vector<HostAndPort> result;
        for (const auto& hostAndPort : _testUri.getServers()) {
            result.push_back(hostAndPort);
        }
        return result;
    }

    BSONObj _jsonTest;
    std::string _testName;
    MongoURI _testUri;
    std::string _testFilePath;
    TopologyType _initialType;
    boost::optional<std::string> _replicaSetName;
    std::vector<TestCasePhase> _testPhases;
};

/**
 * This class runs (potentially) multiple json tests and reports their results.
 */
class SdamJsonTestRunner {
public:
    SdamJsonTestRunner(std::string testDirectory, std::vector<std::string> testFilters)
        : _testFiles(scanTestFiles(testDirectory, testFilters)) {}

    std::vector<JsonTestCase::TestCaseResult> runTests() {
        std::vector<JsonTestCase::TestCaseResult> results;
        const auto testFiles = getTestFiles();
        for (auto jsonTest : testFiles) {
            auto testCase = JsonTestCase(jsonTest);
            try {
                LOGV2(20208, "### Executing Test Case ###", "test"_attr = testCase.Name());
                results.push_back(testCase.execute());
            } catch (const DBException& ex) {
                std::stringstream error;
                error << "Exception while executing " << jsonTest.string() << ": " << ex.toString();
                std::string errorStr = error.str();
                results.push_back(JsonTestCase::TestCaseResult{
                    {TestCasePhase::PhaseResult{{std::make_pair("exception", errorStr)}, 0}},
                    jsonTest.string(),
                    testCase.Name()});
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
            LOGV2(20209, "### Failed Test Results ###");
        }

        for (const auto& result : results) {
            auto file = result.file;
            auto testName = result.name;
            auto phaseResults = result.phaseResults;
            if (result.Success()) {
                ++numSuccess;
            } else {
                LOGV2(20210, "### Test Name ###", "name"_attr = testName);
                LOGV2(20211, "Error in file", "file"_attr = file);
                ++numFailed;
                for (auto phaseResult : phaseResults) {
                    LOGV2(20212, "Phase", "phaseNumber"_attr = phaseResult.phaseNumber);
                    if (!phaseResult.Success()) {
                        for (auto error : phaseResult.errorDescriptions) {
                            LOGV2(20213,
                                  "Errors",
                                  "errorFirst"_attr = error.first,
                                  "errorSecond"_attr = error.second);
                        }
                    }
                }
                LOGV2(20214, "");
            }
        }
        LOGV2(20215,
              "Test cases summary",
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
                LOGV2_DEBUG(20216,
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

    SdamJsonTestRunner testRunner(args.SourceDirectory(), args.TestFilters());
    return testRunner.report(testRunner.runTests());
}
