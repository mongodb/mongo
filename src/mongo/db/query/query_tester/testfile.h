// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/query_tester/test.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/modules.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

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
    const std::filesystem::path& getFilePath() const;
    size_t getFailedQueryCount() const;
    size_t getTestsRun() const;
    const std::string& getQuery(size_t testNum) const;
    const std::vector<Test>& getTests() const;

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
    bool writeAndValidate(ModeOption, WriteOutOptions, ErrorLogLevel, DiffStyle, bool isPartialRun);

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
