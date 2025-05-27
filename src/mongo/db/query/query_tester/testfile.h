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

#include "mongo/stdx/unordered_map.h"

#include <fstream>
#include <string>
#include <vector>

#include "test.h"

namespace mongo::query_tester {

static constexpr auto kMinTestNum = size_t{0};
static constexpr auto kMaxTestNum = std::numeric_limits<size_t>::max();
inline constexpr auto kTmpFailureFile = "tmp_failed_queries";

/**
 * A class representing a test file. A test file can have a number of formats (subclassed below).
 * However, all are expected to maintain the same approach to denoting the beginning of an
 * individual test.
 * Collections are specified in different files.
 <----- File Format ----->
 TestName
 Collection1Name Collection2Name ...

<----- End File Format ----->
 */

class QueryFile {
public:
    QueryFile(std::filesystem::path filePath,
              const bool optimizationsOff,
              const OverrideOption overrideOption = OverrideOption::None)
        : _filePath(filePath),
          _optimizationsOff(optimizationsOff),
          _overrideOption(overrideOption),
          _expectedPath(std::filesystem::path{filePath}.replace_extension(
              overrideOptionToExtensionPrefix(_overrideOption).get_value_or("") + ".results")),
          _actualPath(std::filesystem::path{filePath}.replace_extension(
              overrideOptionToExtensionPrefix(_overrideOption).get_value_or("") + ".actual")),
          _failedQueryCount(0) {}

    // Delete certain constructors to avoid accidental I/O races.
    QueryFile(const QueryFile&) = delete;

    /**
     * Drops collections in a test file that haven't been dropped as of the previous file.
     */
    void dropStaleCollections(DBClientConnection*,
                              const std::set<CollectionSpec>& prevFileCollections) const;

    std::string generateFailureReport() const;
    const std::vector<CollectionSpec>& getCollectionsNeeded() const;
    size_t getFailedQueryCount() const;
    size_t getTestsRun() const;
    const std::string& getQuery(size_t testNum) const;

    /**
     * Loads or drops then loads the collections needed for the test files depending on the passed
     * in options. Updates the already dropped/loaded collections from the previous file vector.
     */
    void loadCollections(DBClientConnection*,
                         bool dropData,
                         bool loadData,
                         bool createAllIndices,
                         bool ignoreIndexFailures,
                         const std::set<CollectionSpec>& prevFileCollections) const;

    void assertTestNumExists(size_t testNum) const;

    /**
     * Given a featureSet containing a testNum and its corresponding feature information, group the
     * features into higher-level categories and print each individual feature under its category.
     * The output will look like:
     *
     * Query Features:
     *    Category 1:
     *       - Feature
     *       - Feature
     *    Category 2:
     *       - Feature
     *       - Feature
     *    ... and so on
     */
    void displayFeaturesByCategory(const BSONElement& featureSet) const;
    /**
     * Parse a feature file into a BSONObj and process features for each testNum by category.
     */
    void parseFeatureFileToBson(const std::filesystem::path& queryFeaturesFile) const;
    /**
     * Print out failed test numbers and their corresponding queries. Optionally, with the -v
     * (verbose) flag set, also extract and print out metadata about common features across failed
     * queries for an enriched debugging experience.
     */
    void printFailedTestFileHeader() const;
    void printAndExtractFailedQueries(const std::set<size_t>& failedTestNums) const;
    void printFailedQueries(const std::set<size_t>& failedTestNums) const;

    bool readInEntireFile(ModeOption, size_t = kMinTestNum, size_t = kMaxTestNum);
    void runTestFile(DBClientConnection*, ModeOption);

    /**
     * Write out all the non-test information to a string for debug purposes.
     */
    std::string serializeStateForDebug() const;

    /**
     * If 'compare' is set, tests must have results to compare to.
     */
    bool textBasedCompare(const std::filesystem::path&,
                          const std::filesystem::path&,
                          ErrorLogLevel,
                          DiffStyle);

    /**
     * If 'compare' is set, tests must have results to compare to.
     */
    bool writeAndValidate(ModeOption, WriteOutOptions, ErrorLogLevel, DiffStyle);

    bool writeOutAndNumber(std::fstream&, WriteOutOptions);

    std::filesystem::path writeOutFailedQueries(const std::set<size_t>& failedTestNums) const;

    template <bool IncludeComments>
    void writeOutHeader(std::fstream&) const;

protected:
    void parseHeader(std::fstream& fs);
    const std::filesystem::path _filePath;
    std::vector<CollectionSpec> _collectionsNeeded;
    std::string _databaseNeeded;
    std::vector<Test> _tests;
    size_t _testsRun = 0;
    const bool _optimizationsOff;
    const OverrideOption _overrideOption;
    std::filesystem::path _expectedPath;
    std::filesystem::path _actualPath;
    struct {
        std::vector<std::string> preName;
        std::vector<std::string> preCollName;
        std::vector<std::vector<std::string>> preCollFiles;
    } _comments;
    // Stores a mapping between each test number and its associated query string.
    stdx::unordered_map<size_t, std::string> _testNumToQuery;
    size_t _failedQueryCount;
};
}  // namespace mongo::query_tester
