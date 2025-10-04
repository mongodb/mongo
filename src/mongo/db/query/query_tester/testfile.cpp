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

#include "testfile.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/query/util/jparse_util.h"
#include "mongo/util/pcre.h"
#include "mongo/util/shell_exec.h"

#include <algorithm>
#include <ostream>
#include <vector>

#include "command_helpers.h"

namespace mongo::query_tester {
namespace {

// This regex matches geospatial and text indices.
static const auto kAlwaysIncludedIndex =
    pcre::Regex{R"-([{,]\s*("?)[^":,]+\1\s*:\s*"(2d|2dsphere|text)"\s*[},])-"};

void dropCollections(DBClientConnection* const conn,
                     const std::string& dbName,
                     const std::vector<std::string>& collections) {
    auto cmd = BSON("drop" << "");
    for (const auto& collName : collections) {
        auto bob = BSONObjBuilder{};
        bob.append("drop", collName);
        // Allow NamespaceNotFound.
        runCommandAssertOK(conn, bob.done(), dbName, {ErrorCodes::NamespaceNotFound});
    }
}

// Format result set so that each result is on a separate line.
std::string formatResultSet(const BSONObj& obj) {
    auto oss = std::ostringstream{};
    const auto arrayElt = obj.hasField("res") ? obj.getField("res") : obj.firstElement();
    if (arrayElt.type() == BSONType::array) {
        oss << ArrayResult<BSONElement>{arrayElt.Array()};
    } else {
        uasserted(9670433,
                  str::stream{} << "Expected result set to be of type array, but got "
                                << arrayElt.type());
    }
    return oss.str();
}

void moveResultsFile(const std::filesystem::path& actualPath,
                     const std::filesystem::path& filePath,
                     const WriteOutOptions writeOutOpts,
                     const boost::optional<std::string>& overrideExtensionPrefix) {
    switch (writeOutOpts) {
        case WriteOutOptions::kNone: {
            // Clean up.
            std::filesystem::remove(actualPath);
            break;
        }
        case WriteOutOptions::kOnelineResult:
        case WriteOutOptions::kResult: {
            std::filesystem::rename(actualPath,
                                    std::filesystem::path{filePath}.replace_extension(
                                        overrideExtensionPrefix.get_value_or("") + ".results"));
            break;
        }
    }
}

void printFailedTestNumAndQuery(const size_t testNum, const std::string& query) {
    // Print out the failed testId and its corresponding query.
    std::cout << applyBold() << "TestNum: " << applyReset() << testNum << std::endl
              << applyBold() << "Query: " << applyReset() << query << std::endl
              << std::endl;
}

void readAndBuildOrSkipIndexes(DBClientConnection* const conn,
                               const std::string& dbName,
                               const std::string& collName,
                               std::fstream& fs,
                               const bool createAllIndices,
                               const bool ignoreIndexFailures) {
    verifyFileStreamGood(
        fs, std::filesystem::path{collName}, std::string{"Stream not ready to read indexes"});
    auto lineFromFile = std::string{};
    auto bob = BSONObjBuilder{};
    bob.append("createIndexes", collName);
    auto indexBuilder = BSONArrayBuilder{};
    auto hasIndicesToCreate = false;

    readLine(fs, lineFromFile);
    for (auto indexNum = 0; !lineFromFile.empty(); readLine(fs, lineFromFile), ++indexNum) {
        // Do index inclusion checks here.
        if (createAllIndices || kAlwaysIncludedIndex.match(lineFromFile)) {
            hasIndicesToCreate = true;

            const auto& indexObj = fromFuzzerJson(lineFromFile);
            BSONObjBuilder indexBob;

            // Append "key" field containing the index if present.
            if (const auto keyObj = indexObj.getObjectField("key"); !keyObj.isEmpty()) {
                indexBob.append("key", keyObj);
                // Append "options" field containing the indexOptions if present.
                if (const auto optionsObj = indexObj.getObjectField("options");
                    !optionsObj.isEmpty()) {
                    indexBob.appendElements(optionsObj);
                }
            } else {
                // Otherwise, the entire object is just an index key.
                indexBob.append("key", indexObj);
            }

            indexBob.append("name", mongo::str::stream{} << "index_" << indexNum);

            // Ignore index failures means we need to create the indices one at a time.
            if (ignoreIndexFailures) {
                auto localBob = BSONObjBuilder{};
                localBob.append("createIndexes", collName);
                auto localIndexBuilder = BSONArrayBuilder{};
                localIndexBuilder.append(indexBob.done());
                localBob.append("indexes", localIndexBuilder.arr());
                const auto cmd = localBob.done();
                try {
                    runCommandAssertOK(conn, cmd, dbName);
                } catch (const AssertionException& ex) {
                    std::cerr << ex.reason() << std::endl;
                }

                // Reset the flag so that we don't try to create an empty index list at the end.
                hasIndicesToCreate = false;
            } else {
                indexBuilder.append(indexBob.done());
            }
        }
    }

    if (hasIndicesToCreate) {
        bob.append("indexes", indexBuilder.arr());
        const auto cmd = bob.done();
        runCommandAssertOK(conn, cmd, dbName);
    }
}

// Returns true if another batch is required.
bool readAndInsertNextBatch(DBClientConnection* const conn,
                            const std::string& dbName,
                            const std::string& collName,
                            std::fstream& fileToRead) {
    auto bob = BSONObjBuilder{};
    bob.append("insert", collName);
    auto docBuilder = BSONArrayBuilder{};
    auto currentObjSize = 0;
    auto lineFromFile = std::string{};

    // Protect from reading past the end of file. Only read one line at a time for each check here.
    while (!fileToRead.eof()) {
        readLine(fileToRead, lineFromFile);
        // Protect from inserting a malformed empty string as a document. Treat an empty line as an
        // indication to end document insertion.
        if (lineFromFile.empty()) {
            break;
        }
        currentObjSize += lineFromFile.size();
        docBuilder.append(fromFuzzerJson(lineFromFile));
        if (currentObjSize > 100000) {
            bob.append("documents", docBuilder.arr());
            auto cmd = bob.done();
            runCommandAssertOK(conn, cmd, dbName);
            return true;
        }
    }
    // Protect from if the last document put currentObjSize over the hardcoded size limit, causing a
    // flush. This then leaves the BSOn array empty for this final insertion attempt, but inserting
    // an empty array causes an error, so we try to avoid it.
    if (currentObjSize > 0) {
        bob.append("documents", docBuilder.arr());
        auto cmd = bob.done();
        runCommandAssertOK(conn, cmd, dbName);
    } else {
        std::cerr << "Found no further documents to insert, so skipping the insert call."
                  << std::endl;
    }
    return false;
}

bool readAndLoadCollFile(DBClientConnection* const conn,
                         const std::string& dbName,
                         const std::string& collName,
                         const std::filesystem::path& filePath,
                         const bool createAllIndices,
                         const bool ignoreIndexFailures) {
    auto collFile = std::fstream{filePath};
    verifyFileStreamGood(collFile, filePath, "Failed to open file");
    // Read in indexes.
    readAndBuildOrSkipIndexes(
        conn, dbName, collName, collFile, createAllIndices, ignoreIndexFailures);
    for (auto needMore = readAndInsertNextBatch(conn, dbName, collName, collFile);
         needMore && !collFile.eof();
         needMore = readAndInsertNextBatch(conn, dbName, collName, collFile)) {
        verifyFileStreamGood(collFile, filePath, "Failed to read batch");
    }
    return true;
}
}  // namespace

void QueryFile::dropStaleCollections(DBClientConnection* const conn,
                                     const std::set<CollectionSpec>& prevFileCollections) const {
    auto collectionsToDrop = std::vector<std::string>{};
    for (const auto& collSpec : _collectionsNeeded) {
        if (prevFileCollections.find(collSpec) == prevFileCollections.end()) {
            collectionsToDrop.emplace_back(collSpec.collName);
        }
    }
    if (!collectionsToDrop.empty()) {
        dropCollections(conn, _databaseNeeded, collectionsToDrop);
    }
}

const std::vector<CollectionSpec>& QueryFile::getCollectionsNeeded() const {
    return _collectionsNeeded;
}

size_t QueryFile::getFailedQueryCount() const {
    return _failedQueryCount;
}

size_t QueryFile::getTestsRun() const {
    return _testsRun;
}

const std::string& QueryFile::getQuery(const size_t testNum) const {
    return _testNumToQuery.at(testNum);
}

void QueryFile::loadCollections(DBClientConnection* const conn,
                                const bool dropData,
                                const bool loadData,
                                const bool createAllIndices,
                                const bool ignoreIndexFailures,
                                const std::set<CollectionSpec>& prevFileCollections) const {
    if (dropData) {
        dropStaleCollections(conn, prevFileCollections);
    }

    // Set optimization flags if needed.
    if (_optimizationsOff) {
        const auto disablePipeOpt =
            fromFuzzerJson("{configureFailPoint: 'disablePipelineOptimization', mode: 'alwaysOn'}");
        const auto disableMatchOpt = fromFuzzerJson(
            "{configureFailPoint: 'disableMatchExpressionOptimization', mode: 'alwaysOn'}");

        try {
            runCommandAssertOK(conn, disablePipeOpt, "admin");
            runCommandAssertOK(conn, disableMatchOpt, "admin");
        } catch (AssertionException& ex) {
            uassert(9816700,
                    (std::stringstream()
                     << "Setting optimization inhibiting failpoints failed. "
                     << "You may need to restart your server with  --setParameter "
                        "\"enableTestCommands=true\". "
                     << "Or you may need to run with optimizations enabled.\n"
                     << "Failed to disable optimizations. Reason: " << ex.reason())
                        .str(),
                    false);
        }
    }

    if (loadData) {
        // Load collections.
        uassert(9670419,
                "Expected at least one collection to be required. Has the file been read?",
                !_collectionsNeeded.empty());
        // Figure out our path.
        const auto pathPrefix = std::filesystem::path{_filePath}.remove_filename();
        // Deduce collection file.
        for (const auto& collSpec : _collectionsNeeded) {
            if (prevFileCollections.find(collSpec) == prevFileCollections.end()) {
                // Only load a collection if it wasn't marked as loaded by the previous file.
                readAndLoadCollFile(conn,
                                    _databaseNeeded,
                                    collSpec.collName,
                                    collSpec.filePath,
                                    createAllIndices,
                                    ignoreIndexFailures);
            }
        }
    }
}

// Expects 'fs' to be open.
void QueryFile::parseHeader(std::fstream& fs) {
    auto lineFromFile = std::string{};
    _comments.preName = readLine(fs, lineFromFile);
    verifyFileStreamGood(fs, _filePath, "Failed to read header line");
    // The first line of a file is required to be the filename.
    const auto nameNoExtension = getBaseNameFromFilePath(_filePath);
    uassert(9670402,
            str::stream{} << "Expected first test line of " << _filePath.string()
                          << " to match the test name, but got " << lineFromFile,
            nameNoExtension == lineFromFile);
    _comments.preCollName = readLine(fs, lineFromFile);
    uassert(9670411,
            str::stream{} << "Expected single database, got multiple in " << _filePath.string()
                          << ". Databases are " << lineFromFile,
            lineFromFile.find(' ') == std::string::npos);
    // Next line is a single database.
    _databaseNeeded = lineFromFile;

    // Next lines are a set of collection specifications.
    for (_comments.preCollFiles.push_back(readLine(fs, lineFromFile));
         !lineFromFile.empty() && !fs.eof();
         _comments.preCollFiles.push_back(readLine(fs, lineFromFile))) {
        const auto& relativeSpec = toCollectionSpec(lineFromFile);
        _collectionsNeeded.push_back(
            {relativeSpec.collName,
             // Compute the absolute path to the desired .coll file.
             std::filesystem::absolute(
                 std::filesystem::path{_filePath}.replace_filename(relativeSpec.filePath)),
             relativeSpec.rawString});
    }

    // Final header line should be a newline.
    uassert(9670432,
            str::stream{} << "Expected newline at end of header for file " << _filePath.string(),
            lineFromFile.empty());
}

void QueryFile::assertTestNumExists(const size_t testNum) const {
    uassert(9699600,
            str::stream() << "Test " << testNum << " does not exist.",
            _testNumToQuery.find(testNum) != _testNumToQuery.end());
}

void QueryFile::displayFeaturesByCategory(const BSONElement& featureSet) const {
    const auto& testNum = static_cast<size_t>(std::stoull(featureSet.fieldName()));
    assertTestNumExists(testNum);

    printFailedTestNumAndQuery(testNum, getQuery(testNum));

    std::cout << applyBold() << "Query Features:" << applyReset() << std::endl;
    auto groupedFeatures = std::map<std::string, std::vector<std::string>>{};

    for (const auto& feature : featureSet.Obj()) {
        // Only count features that are not null and begin with an allowed prefix.
        if (const auto featureName = feature.fieldName();
            !feature.isNull() && matchesPrefix(featureName)) {
            const auto [category, value] = splitFeature(featureName);
            groupedFeatures[category].push_back(value);
        }
    }

    for (auto& [category, values] : groupedFeatures) {
        // Display individual features under their broader categories.
        std::cout << "   " << applyBold() << category << ":" << applyReset() << std::endl;
        std::sort(values.begin(), values.end());
        for (const auto& value : values) {
            if (!value.empty()) {
                std::cout << "      - " << value << std::endl;
            }
        }
    }
    std::cout << std::endl << std::endl;
}

void QueryFile::parseFeatureFileToBson(const std::filesystem::path& queryFeaturesFile) const {
    auto ifs = std::ifstream{queryFeaturesFile};
    tassert(9699500,
            "Expected file to be open and ready for reading, but it wasn't",
            ifs.is_open() && ifs.good());

    const auto jsonStr =
        std::string{std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>()};
    const auto& obj = fromjson(jsonStr);
    for (const auto& featureSet : obj) {
        // For each testNum, group and display features by category.
        displayFeaturesByCategory(featureSet);
    }
}

void QueryFile::printFailedTestFileHeader() const {
    std::cout << applyRed() << "------------------------------------------------------------"
              << applyReset() << std::endl
              << applyCyan() << "FAIL: " << _filePath.string() << applyReset() << std::endl;
}

void QueryFile::printAndExtractFailedQueries(const std::set<size_t>& failedTestNums) const {
    const auto& failPath = writeOutFailedQueries(failedTestNums);
    const auto& repoRoot = getMongoRepoRoot();

    // Extract failed test file to a pickle of dataframes.
    const auto pickleFailedTestCmd = std::stringstream{}
        << "python3 src/mongo/db/query/query_tester/scripts/extract_failed_test_to_pickle.py "
        << repoRoot << " " << kFeatureExtractorDir << " " << kTmpFailureFile << " "
        << failPath.string();

    if (executeShellCmd(pickleFailedTestCmd.str()).isOK()) {
        // Convert pickle file to an intermediate json file for feature processing.
        const auto pickleToJsonCmd = std::stringstream{}
            << "python3 src/mongo/db/query/query_tester/scripts/extract_pickle_to_json.py "
            << repoRoot << " " << kFeatureExtractorDir << " " << kTmpFailureFile;

        const auto queryFeaturesFile =
            std::filesystem::path{(std::stringstream{} << "src/mongo/db/query/query_tester/"
                                                       << kTmpFailureFile << ".json")
                                      .str()};

        if (executeShellCmd(pickleToJsonCmd.str()).isOK()) {
            // Parse, process, and pretty print query features by category.
            parseFeatureFileToBson(queryFeaturesFile);
        } else {
            uasserted(
                9836400,
                str::stream{}
                    << "Failed to extract pickle file to json for feature processing for file "
                    << failPath.string());
        }

        // Clean up intermediate files on success.
        std::filesystem::remove(queryFeaturesFile);
        std::filesystem::remove(failPath);
    } else {
        // No clean up on failure.
        uasserted(9699501,
                  str::stream{}
                      << "Failed to extract " << failPath.string()
                      << " to pickle. To manually retry, run `python3 "
                         "src/mongo/db/query/query_tester/scripts/extract_failed_test_to_pickle.py "
                         "<mongo_repo_root> <feature_extractor_dir> <output_prefix> <path to .fail "
                         "file>` from the mongo repo root.");
    }
}

void QueryFile::printFailedQueries(const std::set<size_t>& failedTestNums) const {
    // Print the failed queries without any metadata extraction for feature processing.
    printFailedTestFileHeader();
    for (const auto& testNum : failedTestNums) {
        assertTestNumExists(testNum);
        printFailedTestNumAndQuery(testNum, getQuery(testNum));
    }
}

bool QueryFile::readInEntireFile(const ModeOption mode,
                                 const size_t startRange,
                                 const size_t endRange) {
    // Open read only.
    auto fs = std::fstream{_filePath, std::fstream::in};
    verifyFileStreamGood(fs, _filePath, "Failed to open file");

    // Read the header.
    parseHeader(fs);

    // The rest of the file is tests.
    auto partialTestRun = false;
    auto nextTestNum = size_t{0};
    while (!fs.eof()) {
        auto currByteOffset = fs.tellg();
        try {
            auto test = Test::parseTest(fs, mode, _optimizationsOff, nextTestNum, _overrideOption);
            // The test number (localTestNum) from the file may differ from the expected sequence
            // (nextTestNum).
            auto localTestNum = test.getTestNum();
            test.setDB(_databaseNeeded);
            if (localTestNum >= startRange && localTestNum <= endRange) {
                _tests.push_back(test);
            } else {
                partialTestRun = true;
            }
            nextTestNum = localTestNum + 1;
        } catch (AssertionException& ex) {
            // To see which lines failed, run "tail -c +<byte-offset> <full-path-to-test>"
            ex.addContext(str::stream{} << "Failed to read test at byte " << currByteOffset);
            if (ex.code() == 9948600) {
                // Fail the entire testfile if we hit an invalid localTestNum that violates the
                // monotonically increasing expectation.
                throw;
            } else {
                std::cerr << _filePath.string() << std::endl << ex.reason() << std::endl;
            }
        }
    }
    // Close the file.
    fs.close();

    // If we're not running all tests, print the expected results of the narrowed set of tests to a
    // temporary results file.
    if (mode == ModeOption::Compare && partialTestRun) {
        const auto narrowedPath = std::filesystem::path{_expectedPath}.concat(".narrowed");
        auto narrowedStream = std::fstream{narrowedPath, std::ios::out | std::ios::trunc};

        auto testsWithResults = QueryFile{_expectedPath, false, _overrideOption};
        testsWithResults.readInEntireFile(ModeOption::Normalize, startRange, endRange);
        testsWithResults.writeOutAndNumber(narrowedStream, WriteOutOptions::kResult);
        narrowedStream.close();
        _expectedPath = narrowedPath;
    }

    return true;
}

void QueryFile::runTestFile(DBClientConnection* conn, const ModeOption mode) {
    _testsRun = 0;
    for (auto& test : _tests) {
        // Skip tests that have already been failed during input/parsing.
        if (test.getErrorMessage()) {
            continue;
        }
        try {
            test.runTestAndRecord(conn, mode);
            ++_testsRun;
        } catch (const AssertionException& ex) {
            std::cerr << std::endl << _filePath.string() << std::endl << ex.reason() << std::endl;
        }
    }
}

/**
 * Write out all the non-test information to a string for debug purposes.
 */
std::string QueryFile::serializeStateForDebug() const {
    auto ss = std::stringstream{};
    ss << "_filePath: " << _filePath.string() << " | db: " << _databaseNeeded + " | ";
    for (const auto& coll : _collectionsNeeded) {
        ss << coll.rawString << " , ";
    }
    ss << " NumTests: " << _tests.size() << " | ";
    return ss.str();
}

bool QueryFile::textBasedCompare(const std::filesystem::path& expectedPath,
                                 const std::filesystem::path& actualPath,
                                 const ErrorLogLevel errorLogLevel,
                                 const DiffStyle diffStyle) {
    if (const auto& diffOutput = gitDiff(expectedPath, actualPath, diffStyle);
        !diffOutput.empty()) {
        // Write out the diff output.
        std::cout << diffOutput << std::endl;

        const auto& failedTestNums = getFailedTestNums(diffOutput);
        if (!failedTestNums.empty()) {
            if (errorLogLevel == ErrorLogLevel::kExtractFeatures) {
                printAndExtractFailedQueries(failedTestNums);
            } else if (errorLogLevel == ErrorLogLevel::kVerbose) {
                printFailedQueries(failedTestNums);
            }
            _failedQueryCount += failedTestNums.size();
        }

        // No cleanup on failure.
        return false;
    } else {
        // Clean up on success.
        std::filesystem::remove(actualPath);
        // This might be clearer than `return result.empty()`.
        return true;
    }
}

bool QueryFile::writeAndValidate(const ModeOption mode,
                                 const WriteOutOptions writeOutOpts,
                                 const ErrorLogLevel errorLogLevel,
                                 const DiffStyle diffStyle) {
    // Set up the text-based diff environment.
    std::filesystem::create_directories(std::filesystem::absolute(_actualPath).parent_path());
    auto actualStream = std::fstream{_actualPath, std::ios::out | std::ios::trunc};
    // Default to kResult for comparisons unless another write out option is specified.
    writeOutAndNumber(actualStream,
                      writeOutOpts == WriteOutOptions::kNone ? WriteOutOptions::kResult
                                                             : writeOutOpts);
    actualStream.close();

    // One big comparison, all at once.
    if (mode == ModeOption::Compare ||
        (mode == ModeOption::Normalize && writeOutOpts == WriteOutOptions::kNone)) {
        return textBasedCompare(_expectedPath, _actualPath, errorLogLevel, diffStyle);
    } else {
        const bool includeResults = writeOutOpts == WriteOutOptions::kResult ||
            writeOutOpts == WriteOutOptions::kOnelineResult;
        uassert(9670450,
                "Must have run query file before writing out result file",
                !includeResults || _testsRun == _tests.size() || _testsRun > 0);
        moveResultsFile(
            _actualPath, _filePath, writeOutOpts, overrideOptionToExtensionPrefix(_overrideOption));
        return _testsRun == _tests.size();
    }
}

bool QueryFile::writeOutAndNumber(std::fstream& fs, const WriteOutOptions opt) {
    // Write out header with comments.
    writeOutHeader<true>(fs);
    // Newline after the header is included in the write-out before each test.

    // Write out each test.
    for (const auto& test : _tests) {
        _testNumToQuery[test.getTestNum()] = test.getTestLine();
        // Newline before each test write-out.
        fs << std::endl;
        test.writeToStream(fs, opt, test.getErrorMessage());
    }

    return true;
}

std::filesystem::path QueryFile::writeOutFailedQueries(
    const std::set<size_t>& failedTestNums) const {
    auto failPath = std::filesystem::path{_filePath}.replace_extension(".fail");
    auto ofs = std::fstream{failPath, std::ios::out | std::ios::trunc};

    // Write out header without comments.
    writeOutHeader<false>(ofs);

    // Print and write the failed queries to a temp file for feature processing.
    for (const auto& testNum : failedTestNums) {
        assertTestNumExists(testNum);
        ofs << std::endl << testNum << std::endl << getQuery(testNum) << std::endl;
    }
    ofs.close();
    return failPath;
}

template <bool IncludeComments>
void QueryFile::writeOutHeader(std::fstream& fs) const {
    // Write the test name, without extension.
    auto nameNoExtension = getBaseNameFromFilePath(_filePath);
    if constexpr (IncludeComments) {
        for (const auto& comment : _comments.preName) {
            fs << comment << std::endl;
        }
    }
    fs << nameNoExtension << std::endl;

    // Write the database name.
    if constexpr (IncludeComments) {
        for (const auto& comment : _comments.preCollName) {
            fs << comment << std::endl;
        }
    }
    fs << _databaseNeeded << std::endl;

    if constexpr (IncludeComments) {
        // Interleave comments and coll file lines.
        auto commentItr = _comments.preCollFiles.begin();
        for (const auto& coll : _collectionsNeeded) {
            if (commentItr != _comments.preCollFiles.end()) {
                for (const auto& comment : *commentItr) {
                    fs << comment << std::endl;
                }
                ++commentItr;
            }
            fs << coll.rawString << std::endl;
        }

        // Drain the remaining comments. In practice, there should only be at most one more entry in
        // _comments.preCollFiles than in _collectionsNeeded.
        for (; commentItr != _comments.preCollFiles.end(); ++commentItr) {
            for (const auto& comment : *commentItr) {
                fs << comment << std::endl;
            }
        }
    } else {
        // Write out collection files.
        for (const auto& coll : _collectionsNeeded) {
            fs << coll.rawString << std::endl;
        }
    }
}
}  // namespace mongo::query_tester
