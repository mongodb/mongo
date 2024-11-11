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
#include "mongo/db/query/util/jparse_util.h"

namespace queryTester {

namespace {
void runCommandAssertOK(mongo::DBClientConnection* conn,
                        mongo::BSONObj command,
                        std::string& db,
                        std::vector<int> acceptableErrorCodes = {}) {
    auto cmdResponse = commandHelpers::runCommand(conn, db, command);
    if (cmdResponse.getField("ok").trueValue()) {
        return;
    }
    for (const auto& error : acceptableErrorCodes) {
        if (cmdResponse.getField("code").safeNumberInt() == error) {
            return;
        }
    }
    uasserted(9670420,
              mongo::str::stream()
                  << "Expected OK command result from " << command << " but got " << cmdResponse);
}

// Returns true if another batch is required.
bool readAndInsertNextBatch(mongo::DBClientConnection* conn,
                            std::string& dbName,
                            std::string& collName,
                            std::fstream& fileToRead) {
    mongo::BSONObjBuilder bob;
    bob.append("insert", collName);
    mongo::BSONArrayBuilder docBuilder;
    int currentObjSize = 0;
    std::string lineFromFile;
    fileHelpers::readLine(fileToRead, lineFromFile);
    while (!fileToRead.eof()) {
        currentObjSize += lineFromFile.size();
        docBuilder.append(mongo::fromFuzzerJson(lineFromFile));
        if (currentObjSize > 100000) {
            bob.append("documents", docBuilder.arr());
            auto cmd = bob.done();
            runCommandAssertOK(conn, cmd, dbName);
            return true;
        }
        fileHelpers::readLine(fileToRead, lineFromFile);
    }
    bob.append("documents", docBuilder.arr());
    auto cmd = bob.done();
    runCommandAssertOK(conn, cmd, dbName);
    return false;
}

void readAndBuildIndexes(mongo::DBClientConnection* conn,
                         std::string& dbName,
                         std::string& collName,
                         std::fstream& fs) {
    fileHelpers::verifyFileStreamGood(
        fs, std::filesystem::path{collName}, std::string("Stream not ready to read indexes"));
    std::string lineFromFile;
    mongo::BSONObjBuilder bob;
    mongo::BSONArrayBuilder indexBuilder;
    bob.append("createIndexes", collName);

    fileHelpers::readLine(fs, lineFromFile);
    size_t indexNum = 0;
    while (lineFromFile.size() != 0) {
        mongo::BSONObjBuilder indexBob;
        indexBob.append("key", mongo::fromFuzzerJson(lineFromFile));
        indexBob.append("name", mongo::str::stream() << "index_" << indexNum++);
        indexBuilder.append(indexBob.done());
        fileHelpers::readLine(fs, lineFromFile);
    }

    bob.append("indexes", indexBuilder.arr());
    auto cmd = bob.done();
    runCommandAssertOK(conn, cmd, dbName);
}

bool readAndLoadCollFile(mongo::DBClientConnection* conn,
                         std::string& dbName,
                         std::string& collName,
                         std::filesystem::path& filePath) {
    auto collFile = std::fstream{filePath};
    fileHelpers::verifyFileStreamGood(collFile, filePath, "Failed to open file");
    // Read in indexes.
    readAndBuildIndexes(conn, dbName, collName, collFile);
    auto needMore = readAndInsertNextBatch(conn, dbName, collName, collFile);
    while (needMore) {
        fileHelpers::verifyFileStreamGood(collFile, filePath, "Failed to read batch");
        needMore = readAndInsertNextBatch(conn, dbName, collName, collFile);
    }
    return true;
}

void dropCollections(mongo::DBClientConnection* conn,
                     std::string& dbName,
                     std::vector<std::string> collections) {
    mongo::BSONObj cmd = BSON("drop"
                              << "");
    for (const auto& coll : collections) {
        mongo::BSONObjBuilder bob;
        bob.append("drop", std::get<0>(fileHelpers::getCollAndFileName(coll)));
        // Allow NamespaceNotFound.
        runCommandAssertOK(conn, bob.done(), dbName, {26});
    }
}

// Format result set so that each result is on a separate line.
std::string formatResultSet(const mongo::BSONObj& obj) {
    std::ostringstream oss;
    const auto arrayElt = obj.hasField("res") ? obj.getField("res") : obj.firstElement();
    if (arrayElt.type() == mongo::Array) {
        oss << fileHelpers::ArrayResult<mongo::BSONElement>{arrayElt.Array()};
    } else {
        uasserted(9670433,
                  mongo::str::stream()
                      << "Expected result set to be of type array, but got " << arrayElt.type());
    }
    return oss.str();
}
}  // namespace

void QueryFile::dropStaleCollections(mongo::DBClientConnection* conn,
                                     std::set<std::string>& prevFileCollections) {
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

void QueryFile::loadCollections(mongo::DBClientConnection* conn,
                                CollectionInitOptions opt,
                                std::set<std::string>& prevFileCollections) {
    if (opt == CollectionInitOptions::kNone) {
        return;
    }
    switch (opt) {
        case CollectionInitOptions::kNone:
            return;
        case CollectionInitOptions::kDrop:
            dropStaleCollections(conn, prevFileCollections);
            return;
        case CollectionInitOptions::kDropAndLoad:
            dropStaleCollections(conn, prevFileCollections);
            [[fallthrough]];
        case CollectionInitOptions::kLoad:;
            // Pass
    }
    // Load collections.
    uassert(9670419,
            "Expected at least one collection to be required. Has the file been read?",
            _collectionsNeeded.size() != 0);
    // Figure out our path.
    auto pathPrefix = std::filesystem::path{_filePath}.remove_filename();
    // Deduce collection file.
    for (const auto& collSpec : _collectionsNeeded) {
        auto [collName, fileName] = fileHelpers::getCollAndFileName(collSpec);
        auto fullPath = pathPrefix / fileName;
        if (prevFileCollections.find(collName) == prevFileCollections.end()) {
            // Only load a collection if it wasn't marked as loaded by the previous file.
            readAndLoadCollFile(conn, _databaseNeeded, collName, fullPath);
        }
    }
}

// Expects 'fs' to be open.
void QueryFile::parseHeader(std::fstream& fs) {
    std::string lineFromFile;
    _comments.preName = fileHelpers::readLine(fs, lineFromFile);
    fileHelpers::verifyFileStreamGood(fs, _filePath, "Failed to read header line");
    // The first line of a file is required to be the filename.
    auto nameNoExtension = fileHelpers::getTestNameFromFilePath(_filePath);
    uassert(9670402,
            mongo::str::stream() << "Expected first test line of " << _filePath.string()
                                 << " to match the test name, but got " << nameNoExtension,
            lineFromFile.compare(nameNoExtension) == 0);
    _comments.preCollName = fileHelpers::readLine(fs, lineFromFile);
    uassert(9670411,
            mongo::str::stream() << "Expected single database, got multiple in "
                                 << _filePath.string() << ". Databases are " << lineFromFile,
            lineFromFile.find(' ') == std::string::npos);
    // Next line is a single database.
    _databaseNeeded = lineFromFile;

    // Next lines are a set of collection specifications.
    for (_comments.preCollFiles.push_back(fileHelpers::readLine(fs, lineFromFile));
         lineFromFile != "" && !fs.eof();
         _comments.preCollFiles.push_back(fileHelpers::readLine(fs, lineFromFile))) {
        _collectionsNeeded.push_back(lineFromFile);
    }

    // Final header line should be a newline.
    uassert(9670432,
            mongo::str::stream() << "Expected newline at end of header for file "
                                 << _filePath.string(),
            lineFromFile == "");
}

bool QueryFile::readInEntireFile(ModeOption mode) {
    // Open File
    std::fstream fs;
    // Open read only.
    fs.open(_filePath, std::fstream::in);
    fileHelpers::verifyFileStreamGood(fs, _filePath, "Failed to open file");

    // Read the header.
    parseHeader(fs);

    size_t testNum = 0;
    // The rest of the file is tests.
    while (!fs.eof()) {
        try {
            _tests.push_back(Test::parseTest(fs, mode, testNum));
            _tests.back().setDB(_databaseNeeded);

        } catch (mongo::AssertionException& ex) {
            fs.close();
            ex.addContext(mongo::str::stream() << "Failed to read test number " << _tests.size());
            throw;
        }
        ++testNum;
    }
    // Close the file.
    fs.close();
    return true;
}

void QueryFile::runTestFile(mongo::DBClientConnection* conn, const ModeOption mode) {
    _testsRun = 0;
    for (auto testNum = size_t{0}; testNum <= _tests.size() - 1; ++testNum, ++_testsRun) {
        // Ensure that the test exists and is set up correctly based on the run mode.
        uassert(9670421,
                mongo::str::stream()
                    << "Attempted to run test number " << testNum << " but file "
                    << _filePath.string() << " only has " << _tests.size() << " tests.",
                testNum < _tests.size());
        auto& test = _tests[testNum];
        test.runTestAndRecord(conn, mode);
    }
}

namespace {
// Note that filePath is intended to be a copy.
void moveResultsFile(const std::filesystem::path& actualPath,
                     std::filesystem::path filePath,  // Intentionally mutable
                     const WriteOutOptions writeOutOpts) {
    switch (writeOutOpts) {
        case WriteOutOptions::kNone: {
            // Clean up.
            std::filesystem::remove(actualPath);
            break;
        }
        case WriteOutOptions::kOnelineResult:
        case WriteOutOptions::kResult: {
            std::filesystem::rename(actualPath, filePath.replace_extension(".results"));
            break;
        }
    }
}

bool textBasedCompare(const std::filesystem::path& expectedPath,
                      const std::filesystem::path& actualPath) {
    if (auto result = fileHelpers::gitDiff(expectedPath, actualPath); !result.empty()) {
        std::cout << result << std::endl;
        // No cleanup on failure.
        return false;
    } else {
        // Clean up on success.
        std::filesystem::remove(actualPath);
        // This might be clearer than `return result.empty()`.
        return true;
    }
}
}  // namespace

bool QueryFile::writeAndValidate(const ModeOption mode, const WriteOutOptions writeOutOpts) {
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
        return textBasedCompare(_expectedPath, _actualPath);
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
    // Write out the header.
    auto nameNoExtension = fileHelpers::getTestNameFromFilePath(_filePath);
    for (const auto& comment : _comments.preName) {
        fs << comment << std::endl;
    }
    fs << nameNoExtension << std::endl;
    for (const auto& comment : _comments.preCollName) {
        fs << comment << std::endl;
    }
    fs << _databaseNeeded << std::endl;

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

    // Newline after the header.
    fs << std::endl;

    // Write out each test.
    for (size_t i = 0; i < _tests.size(); ++i) {
        _tests[i].writeToStream(fs, opt);
        // There should be one empty line after every test.
        if (i != _tests.size() - 1) {
            fs << std::endl;
        }
    }

    return true;
}

}  // namespace queryTester
