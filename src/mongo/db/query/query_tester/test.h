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

#pragma once

#include "mongo/client/dbclient_connection.h"
#include "mongo/db/query/util/jparse_util.h"
#include "mongo/shell/shell_utils.h"

#include <filesystem>
#include <fstream>
#include <string>

#include "file_helpers.h"

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
