// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/client/dbclient_connection.h"
#include "mongo/db/query/query_tester/file_helpers.h"
#include "mongo/db/query/util/jparse_util.h"
#include "mongo/shell/shell_utils.h"
#include "mongo/util/modules.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace mongo::query_tester {
using shell_utils::NormalizationOpts;
using shell_utils::NormalizationOptsSet;

enum class ModeOption { Run, Compare, Normalize };
ModeOption stringToModeOption(const std::string&);

enum class OverrideOption { None, QueryShapeHash };
OverrideOption stringToOverrideOption(const std::string&);
boost::optional<std::string> overrideOptionToExtensionPrefix(OverrideOption);

class Test {
public:
    Test(const std::string& testLine,
         const bool optimizationsOff,
         const size_t testNum,
         boost::optional<std::string> testName,
         std::vector<std::string>&& preTestComments,
         std::vector<std::string>&& preQueryComments,
         std::vector<std::string>&& postQueryComments,
         std::vector<std::string>&& postTestComments,
         std::vector<BSONObj>&& expectedResult = {},
         const OverrideOption overrideOption = OverrideOption::None)
        : _testLine(testLine),
          _optimizationsOff(optimizationsOff),
          _overrideOption(overrideOption),
          _testNum(testNum),
          _testName(testName),
          _comments({preTestComments, preQueryComments, postQueryComments, postTestComments}),
          _expectedResult(std::move(expectedResult)) {
        parseTestQueryLine();
    }

    /**
     * Returns all actual results, including those that are not contained in the first batch.
     */
    std::vector<BSONObj> getAllResults(DBClientConnection* conn, const BSONObj& result);

    std::string getTestLine() const;

    size_t getTestNum() const;

    boost::optional<std::string> getErrorMessage() const;

    const BSONObj& getQuery() const;

    /**
     * Compute the normalized version of the input set.
     */
    static std::vector<std::string> normalize(const std::vector<BSONObj>&, NormalizationOptsSet);

    static NormalizationOptsSet parseResultType(const std::string& type);

    /**
     * Parses a single test definition. This includes the number and name line, the comment line(s)
     * and the actual test command. nextTestNum = prevTestNum + 1, and will be overriden if a user
     * specifies a different testNum so long as it's greater than the previous.
     * Expects the file stream to be open and allow reading.
    <----- Test Format ----->
    <testNumber> <testName>
    Comments on the test, any number of lines
    <testMode> {commandObj}
    <result if result file>
    <----- End Test Format ----->
     */
    static Test parseTest(
        std::fstream&, ModeOption, bool optimizationsOff, size_t nextTestNum, OverrideOption);

    /**
     * Runs the test and records the result returned by the server.
     */
    void runTestAndRecord(DBClientConnection*, ModeOption);

    void setDB(const std::string& db);

    void writeToStream(std::fstream&,
                       WriteOutOptions = WriteOutOptions::kNone,
                       const boost::optional<std::string>& errorMessage = boost::none) const;

private:
    void parseTestQueryLine();
    std::string _testLine;
    const bool _optimizationsOff;
    const OverrideOption _overrideOption;
    size_t _testNum;
    boost::optional<std::string> _testName;
    struct {
        std::vector<std::string> preTest;
        std::vector<std::string> preQuery;
        std::vector<std::string> postQuery;
        std::vector<std::string> postTest;
    } _comments;
    std::vector<BSONObj> _expectedResult = {};
    std::vector<std::string> _normalizedResult = {};
    NormalizationOptsSet _testType = {};
    BSONObj _query = {};
    boost::optional<std::string> _errorMessage;
    std::string _db = {};
};

}  // namespace mongo::query_tester
