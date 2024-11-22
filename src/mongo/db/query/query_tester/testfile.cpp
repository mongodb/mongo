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

#include <algorithm>
#include <ostream>
#include <vector>

#include "command_helpers.h"
#include "mongo/base/error_codes.h"
#include "mongo/db/query/util/jparse_util.h"
#include "mongo/util/shell_exec.h"

namespace mongo::query_tester {
namespace {
void runCommandAssertOK(DBClientConnection*,
                        const BSONObj& command,
                        const std::string& db,
                        std::vector<ErrorCodes::Error> acceptableErrorCodes = {});

void dropCollections(DBClientConnection* const conn,
                     const std::string& dbName,
                     const std::vector<std::string>& collections) {
    auto cmd = BSON("drop"
                    << "");
    for (const auto& coll : collections) {
        auto bob = BSONObjBuilder{};
        bob.append("drop", std::get<0>(getCollAndFileName(coll)));
        // Allow NamespaceNotFound.
        runCommandAssertOK(conn, bob.done(), dbName, {ErrorCodes::NamespaceNotFound});
    }
}

// Format result set so that each result is on a separate line.
std::string formatResultSet(const BSONObj& obj) {
    auto oss = std::ostringstream{};
    const auto arrayElt = obj.hasField("res") ? obj.getField("res") : obj.firstElement();
    if (arrayElt.type() == Array) {
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
                     const WriteOutOptions writeOutOpts) {
    switch (writeOutOpts) {
        case WriteOutOptions::kNone: {
            // Clean up.
            std::filesystem::remove(actualPath);
            break;
        }
        case WriteOutOptions::kOnelineResult:
        case WriteOutOptions::kResult: {
            std::filesystem::rename(actualPath,
                                    std::filesystem::path{filePath}.replace_extension(".results"));
            break;
        }
    }
}

void readAndBuildIndexes(DBClientConnection* const conn,
                         const std::string& dbName,
                         const std::string& collName,
                         std::fstream& fs) {
    verifyFileStreamGood(
        fs, std::filesystem::path{collName}, std::string{"Stream not ready to read indexes"});
    auto lineFromFile = std::string{};
    auto bob = BSONObjBuilder{};
    bob.append("createIndexes", collName);
    auto indexBuilder = BSONArrayBuilder{};

    readLine(fs, lineFromFile);
    for (auto indexNum = 0; !lineFromFile.empty(); readLine(fs, lineFromFile), ++indexNum) {
        const auto& indexObj = fromFuzzerJson(lineFromFile);
        BSONObjBuilder indexBob;

        // Append "key" field containing the index if present.
        if (const auto keyObj = indexObj.getObjectField("key"); !keyObj.isEmpty()) {
            indexBob.append("key", keyObj);
            // Append "options" field containing the indexOptions if present.
            if (const auto optionsObj = indexObj.getObjectField("options"); !optionsObj.isEmpty()) {
                indexBob.appendElements(optionsObj);
            }
        } else {
            // Otherwise, the entire object is just an index key.
            indexBob.append("key", indexObj);
        }

        indexBob.append("name", mongo::str::stream{} << "index_" << indexNum);
        indexBuilder.append(indexBob.done());
    }

    bob.append("indexes", indexBuilder.arr());
    const auto cmd = bob.done();
    runCommandAssertOK(conn, cmd, dbName);
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
    for (readLine(fileToRead, lineFromFile); !fileToRead.eof();
         readLine(fileToRead, lineFromFile)) {
        currentObjSize += lineFromFile.size();
        docBuilder.append(fromFuzzerJson(lineFromFile));
        if (currentObjSize > 100000) {
            bob.append("documents", docBuilder.arr());
            auto cmd = bob.done();
            runCommandAssertOK(conn, cmd, dbName);
            return true;
        }
    }
    bob.append("documents", docBuilder.arr());
    auto cmd = bob.done();
    runCommandAssertOK(conn, cmd, dbName);
    return false;
}

bool readAndLoadCollFile(DBClientConnection* const conn,
                         const std::string& dbName,
                         const std::string& collName,
                         const std::filesystem::path& filePath) {
    auto collFile = std::fstream{filePath};
    verifyFileStreamGood(collFile, filePath, "Failed to open file");
    // Read in indexes.
    readAndBuildIndexes(conn, dbName, collName, collFile);
    for (auto needMore = readAndInsertNextBatch(conn, dbName, collName, collFile); needMore;
         needMore = readAndInsertNextBatch(conn, dbName, collName, collFile)) {
        verifyFileStreamGood(collFile, filePath, "Failed to read batch");
    }
    return true;
}

void runCommandAssertOK(DBClientConnection* const conn,
                        const BSONObj& command,
                        const std::string& db,
                        const std::vector<ErrorCodes::Error> acceptableErrorCodes) {
    auto cmdResponse = runCommand(conn, db, command);
    if (cmdResponse.getField("ok").trueValue()) {
        return;
    }
    for (const auto& error : acceptableErrorCodes) {
        if (error == cmdResponse.getField("code").safeNumberInt()) {
            return;
        }
    }
    uasserted(9670420,
              str::stream{} << "Expected OK command result from " << command << " but got "
                            << cmdResponse);
}
}  // namespace

void QueryFile::dropStaleCollections(DBClientConnection* const conn,
                                     const std::set<std::string>& prevFileCollections) const {
    std::vector<std::string> collectionsToDrop;
    for (const auto& collFileName : _collectionsNeeded) {
        if (prevFileCollections.find(collFileName) == prevFileCollections.end()) {
            collectionsToDrop.emplace_back(collFileName);
        }
    }
    if (!collectionsToDrop.empty()) {
        dropCollections(conn, _databaseNeeded, collectionsToDrop);
    }
}

std::vector<std::string>& QueryFile::getCollectionsNeeded() {
    return _collectionsNeeded;
}

size_t QueryFile::getFailedQueryCount() const {
    return _failedQueryCount;
}

size_t QueryFile::getTestsRun() const {
    return _testsRun;
}

void QueryFile::loadCollections(DBClientConnection* const conn,
                                const bool dropData,
                                const bool loadData,
                                const std::set<std::string>& prevFileCollections) const {
    if (dropData) {
        dropStaleCollections(conn, prevFileCollections);
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
            const auto [collName, fileName] = getCollAndFileName(collSpec);
            const auto fullPath = pathPrefix / fileName;
            if (prevFileCollections.find(collName) == prevFileCollections.end()) {
                // Only load a collection if it wasn't marked as loaded by the previous file.
                readAndLoadCollFile(conn, _databaseNeeded, collName, fullPath);
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
    const auto nameNoExtension = getTestNameFromFilePath(_filePath);
    uassert(9670402,
            str::stream{} << "Expected first test line of " << _filePath.string()
                          << " to match the test name, but got " << nameNoExtension,
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
        _collectionsNeeded.push_back(lineFromFile);
    }

    // Final header line should be a newline.
    uassert(9670432,
            str::stream{} << "Expected newline at end of header for file " << _filePath.string(),
            lineFromFile.empty());
}

void QueryFile::printAndExtractFailedQueries(const std::vector<size_t>& failedQueryIds) const {
    const auto failPath = std::filesystem::path{_filePath}.replace_extension(".fail");
    auto fs = std::fstream{failPath, std::ios::out | std::ios::trunc};
    // Write out header without comments.
    writeOutHeader<false>(fs);

    // Print and write the failed queries to a temp file for feature processing.
    printFailedQueriesHelper(failedQueryIds, &fs);
    fs.close();

    // Extract failed test file to a pickle of dataframes.
    const auto pyCmd = std::stringstream{}
        << "python3 src/mongo/db/query/query_tester/scripts/extract_failed_test_to_pickle.py "
        << getMongoRepoRoot() << " " << kFeatureExtractorDir << " " << kTmpFailureFile << " "
        << std::filesystem::path{failPath}.replace_extension().string();
    if (const auto swRes = shellExec(pyCmd.str(), kShellTimeout, kShellMaxLen, true);
        swRes.isOK()) {
        // Clean up temp .fail file containing failed queries on success.
        std::filesystem::remove(failPath);
    } else {
        // No clean up on failure.
        uasserted(9699501,
                  str::stream{}
                      << "Failed to extract " << failPath.string()
                      << " to pickle. To manually retry, run `python3 "
                         "src/mongo/db/query/query_tester/scripts/extract_failed_test_to_pickle.py "
                         "<mongo_repo_root> <feature_extractor_dir> <output_prefix> <path to .fail "
                         "file without extension>` from the mongo repo root.");
    }
}

void QueryFile::printFailedQueries(const std::vector<size_t>& failedQueryIds) const {
    // Print the failed queries without any metadata extraction for feature processing.
    printFailedQueriesHelper(failedQueryIds);
}

void QueryFile::printFailedQueriesHelper(const std::vector<size_t>& failedTestNums,
                                         std::fstream* fs) const {
    std::cout << applyRed() << "------------------------------------------------------------"
              << applyReset() << std::endl
              << applyCyan() << "FAIL: " << getTestNameFromFilePath(_filePath) << applyReset()
              << std::endl;
    for (const auto& testNum : failedTestNums) {
        uassert(9699600,
                str::stream() << "Test " << testNum << " does not exist.",
                _testNumToQuery.find(testNum) != _testNumToQuery.end());

        // Print out the failed testId and its corresponding query.
        const auto& query = _testNumToQuery.at(testNum);
        std::cout << applyBold() << "TestNum: " << applyReset() << testNum << std::endl
                  << applyBold() << "Query: " << applyReset() << query << std::endl
                  << std::endl;

        // If a file stream is provided, write the failing query to it.
        if (fs) {
            *fs << std::endl << query << std::endl;
        }
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
    bool partialTestRun = false;
    for (size_t testNum = 0; !fs.eof(); ++testNum) {
        try {
            auto test = Test::parseTest(fs, mode, testNum);
            test.setDB(_databaseNeeded);
            if (testNum >= startRange && testNum <= endRange) {
                _tests.push_back(test);
            } else {
                partialTestRun = true;
            }
        } catch (AssertionException& ex) {
            fs.close();
            ex.addContext(str::stream{} << "Failed to read test number " << _tests.size());
            throw;
        }
    }
    // Close the file.
    fs.close();

    // If we're not running all tests, print the expected results of the narrowed set of tests to a
    // temporary results file.
    if (mode == ModeOption::Compare && partialTestRun) {
        const auto narrowedPath = std::filesystem::path{_expectedPath}.concat(".narrowed");
        auto narrowedStream = std::fstream{narrowedPath, std::ios::out | std::ios::trunc};

        auto testsWithResults = QueryFile{_expectedPath};
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
        test.runTestAndRecord(conn, mode);
        ++_testsRun;
    }
}

/**
 * Write out all the non-test information to a string for debug purposes.
 */
std::string QueryFile::serializeStateForDebug() const {
    auto ss = std::stringstream{};
    ss << "_filePath: " << _filePath.string() << " | db: " << _databaseNeeded + " | ";
    for (const auto& coll : _collectionsNeeded) {
        ss << coll << " , ";
    }
    ss << " NumTests: " << _tests.size() << " | ";
    return ss.str();
}

bool QueryFile::textBasedCompare(const std::filesystem::path& expectedPath,
                                 const std::filesystem::path& actualPath,
                                 const ErrorLogLevel errorLogLevel) {
    if (const auto& diffOutput = gitDiff(expectedPath, actualPath); !diffOutput.empty()) {
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
                                 const ErrorLogLevel errorLogLevel) {
    // Set up the text-based diff environment.
    std::filesystem::create_directories(_actualPath.parent_path());
    auto actualStream = std::fstream{_actualPath, std::ios::out | std::ios::trunc};
    // Default to kResult for comparisons unless another write out option is specified.
    writeOutAndNumber(actualStream,
                      writeOutOpts == WriteOutOptions::kNone ? WriteOutOptions::kResult
                                                             : writeOutOpts);
    actualStream.close();

    // One big comparison, all at once.
    if (mode == ModeOption::Compare ||
        (mode == ModeOption::Normalize && writeOutOpts == WriteOutOptions::kNone)) {
        return textBasedCompare(_expectedPath, _actualPath, errorLogLevel);
    } else {
        const bool includeResults = writeOutOpts == WriteOutOptions::kResult ||
            writeOutOpts == WriteOutOptions::kOnelineResult;
        uassert(9670450,
                "Must have run query file before writing out result file",
                !includeResults || _testsRun == _tests.size());
        moveResultsFile(_actualPath, _filePath, writeOutOpts);
        return true;
    }
}

bool QueryFile::writeOutAndNumber(std::fstream& fs, const WriteOutOptions opt) {
    // Write out header with comments.
    writeOutHeader<true>(fs);
    // Newline after the header is included in the write-out before each test.

    // Write out each test.
    for (const auto& test : _tests) {
        // Newline before each test write-out.
        fs << std::endl;
        _testNumToQuery[test.getTestNum()] = test.getTestLine();
        test.writeToStream(fs, opt);
    }

    return true;
}

template <bool IncludeComments>
void QueryFile::writeOutHeader(std::fstream& fs) const {
    // Write the test name, without extension.
    auto nameNoExtension = getTestNameFromFilePath(_filePath);
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
            fs << coll << std::endl;
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
            fs << coll << std::endl;
        }
    }
}
}  // namespace mongo::query_tester
