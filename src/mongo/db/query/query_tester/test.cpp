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

#include "test.h"

#include "mongo/base/string_data_comparator.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/json.h"
#include "mongo/db/query/util/jparse_util.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/shell/shell_utils.h"
#include "mongo/util/pcre.h"

#include <fstream>
#include <thread>

#include "command_helpers.h"

namespace mongo::query_tester {
namespace {
// If a line begins with a mode string, it is a test.
bool isTestLine(const std::string& line) {
    return line.starts_with(":");
}

BSONObj makeExplainCommand(const BSONObj& initialCommand) {
    auto explainCommandBuilder = BSONObjBuilder{};
    // Extract 'apiVersion' and 'apiStrict' properties and remove 'writeConcern' from the inner
    // command object.
    BSONObjBuilder innerCommandBuilder{explainCommandBuilder.subobjStart("explain")};
    for (const auto& element : initialCommand) {
        const auto propName = element.fieldNameStringData();
        if (propName == "writeConcern") {
            continue;
        }
        if (propName == "apiVersion" || propName == "apiStrict") {
            //  'apiVersion' and 'apiStrict' only make sense on the root of the command.
            explainCommandBuilder.append(element);
        } else {
            // Add the rest of the fields to the inner command body.
            innerCommandBuilder.append(element);
        }
    }
    // Append cursor object in case of aggregation.
    if (initialCommand.firstElementFieldNameStringData() == "aggregate" &&
        !initialCommand.hasField("cursor")) {
        innerCommandBuilder.append("cursor", BSONObj());
    }
    innerCommandBuilder.doneFast();
    // Run the explain with the 'queryPlanner' verbosity.
    explainCommandBuilder.append("verbosity", "queryPlanner");
    return explainCommandBuilder.obj();
}

std::string overrideOptionToTestTypeString(const OverrideOption overrideOption) {
    uassert(10387102, "Unexpected override type", overrideOption == OverrideOption::QueryShapeHash);
    return ":queryShapeHash";
}

BSONObj toBSONObj(const std::vector<BSONObj>& objs) {
    auto bob = BSONObjBuilder{};
    bob.append("res", objs.begin(), objs.end());
    return bob.obj();
}
}  // namespace

std::vector<BSONObj> Test::getAllResults(DBClientConnection* const conn, const BSONObj& result) {
    // Early exit in case of explain result.
    if (result.hasField("explainVersion")) {
        return {result};
    }
    const auto actualResult = getResultsFromCommandResponse(result, _testNum);
    const auto id = result.getField("cursor").embeddedObject()["id"].Long();

    auto actualObjs = std::vector<BSONObj>{};
    if (auto actualArr = actualResult.firstElement().Array(); !actualArr.empty()) {
        std::for_each(actualArr.begin(), actualArr.end(), [&actualObjs](const auto& elem) {
            actualObjs.push_back(elem.Obj().getOwned());
        });
    }

    // If cursor ID is 0, all results are in the first batch, and we don't need to call getMore.
    // Otherwise, call getMore() to retrieve the entire result set.
    if (id != 0) {
        auto cursor =
            DBClientCursor(conn,
                           NamespaceStringUtil::deserialize(
                               boost::none,
                               result.getField("cursor").embeddedObject().getStringField("ns"),
                               SerializationContext::stateDefault()),
                           id,
                           false /*isExhaust*/);
        while (cursor.more()) {
            actualObjs.push_back(cursor.nextSafe().getOwned());
        }
    }

    return actualObjs;
}

std::string Test::getTestLine() const {
    return _testLine;
}

size_t Test::getTestNum() const {
    return _testNum;
}

boost::optional<std::string> Test::getErrorMessage() const {
    return _errorMessage;
}

std::vector<std::string> Test::normalize(const std::vector<BSONObj>& objs,
                                         const NormalizationOptsSet opts) {
    const auto numResults = objs.size();
    auto normalized = std::vector<std::string>(numResults);

    const auto numThreads =
        std::min(static_cast<size_t>(std::thread::hardware_concurrency()), numResults);
    auto threads = std::vector<stdx::thread>{};
    threads.reserve(numThreads);

    // Generate partitions and boundaries ahead of time.
    auto partitions = std::vector<size_t>{0};
    auto remainingResults = numResults;
    auto buckets = numThreads;

    for (; remainingResults > 0 && buckets > 0; --buckets) {
        const auto step = remainingResults / buckets;
        partitions.push_back(partitions.back() + step);
        remainingResults -= step;
    }

    // Sanity checks.
    uassert(9670400,
            "Number of results processed by the partitions is not equal to the number of results.",
            partitions.back() == numResults);
    uassert(
        9670401, "Bucket count is not equal to thread count.", partitions.size() == numThreads + 1);

    for (auto sitr = partitions.begin(), eitr = partitions.begin() + 1; eitr != partitions.end();
         ++sitr, ++eitr) {
        threads.emplace_back([&, sitr, eitr]() {
            for (auto index = *sitr; index < *eitr; ++index) {
                // Either perform a direct results comparison or normalize each result in the result
                // set, which may involve sorting the fields and/or arrays within a result object,
                // as well as normalizing numerics to the same type.
                const auto& normalizedObj =
                    (opts == NormalizationOpts::kResults
                         ? objs[index]
                         : shell_utils::normalizeBSONObj(objs[index], opts));
                normalized[index] = normalizedObj.jsonString(ExtendedRelaxedV2_0_0, false, false);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    // Sort the top-level array of results.
    if (shell_utils::isSet(opts, NormalizationOpts::kSortResults)) {
        std::sort(normalized.begin(), normalized.end());
    }

    return normalized;
}

NormalizationOptsSet Test::parseResultType(const std::string& type) {
    static const auto kTypeMap = std::map<std::string, NormalizationOptsSet>{
        {":normalizeFull",
         NormalizationOpts::kSortResults | NormalizationOpts::kSortBSON |
             NormalizationOpts::kSortArrays | NormalizationOpts::kNormalizeNumerics |
             NormalizationOpts::kConflateNullAndMissing},
        {":normalizeNonNull",
         NormalizationOpts::kSortResults | NormalizationOpts::kSortBSON |
             NormalizationOpts::kSortArrays | NormalizationOpts::kNormalizeNumerics},
        {":sortFull",
         NormalizationOpts::kSortResults | NormalizationOpts::kSortBSON |
             NormalizationOpts::kSortArrays | NormalizationOpts::kRoundFloatingPointNumerics},
        {":sortBSONNormalizeNumerics",
         NormalizationOpts::kSortResults | NormalizationOpts::kSortBSON |
             NormalizationOpts::kNormalizeNumerics},
        {":sortBSON",
         NormalizationOpts::kSortResults | NormalizationOpts::kSortBSON |
             NormalizationOpts::kRoundFloatingPointNumerics},
        {":sortResultsNormalizeNumerics",
         NormalizationOpts::kSortResults | NormalizationOpts::kNormalizeNumerics},
        {":normalizeNumerics", NormalizationOpts::kNormalizeNumerics},
        {":normalizeNulls", NormalizationOpts::kConflateNullAndMissing},
        {":sortResults", NormalizationOpts::kSortResults},
        {":results", NormalizationOpts::kResults},
        {":queryShapeHash", NormalizationOpts::kExplain | NormalizationOpts::kQueryShapeHash}};

    if (auto it = kTypeMap.find(type); it != kTypeMap.end()) {
        return it->second;
    } else {
        uasserted(9670456, str::stream{} << "Unexpected test type " << type);
    }
}

Test Test::parseTest(std::fstream& fs,
                     const ModeOption mode,
                     const bool optimizationsOff,
                     const size_t nextTestNum,
                     const OverrideOption overrideOption) {
    auto lineFromFile = std::string{};
    tassert(9670404,
            "Expected file to be open and ready for reading, but it wasn't",
            fs.is_open() && fs.good());
    auto preTestComments = readLine(fs, lineFromFile);
    auto preQueryComments = std::vector<std::string>{};
    auto postQueryComments = std::vector<std::string>{};
    auto postTestComments = std::vector<std::string>{};
    auto localTestNum = nextTestNum;

    const auto [testLine,
                testName] = [&fs, &preQueryComments, &lineFromFile, &nextTestNum, &localTestNum]()
        -> std::tuple<std::string, boost::optional<std::string>> {
        // First line can either be "# NAME" or test. Comments are skipped.
        if (isTestLine(lineFromFile)) {
            return {lineFromFile, boost::none};
        } else {
            auto testName = boost::optional<std::string>{};
            auto iss = std::istringstream{lineFromFile};
            iss >> localTestNum;
            if (iss.eof()) {
                testName = boost::none;
            } else {
                auto testNameString = std::string{};
                iss >> testNameString;
                testName = testNameString;
            }
            uassert(9670451, "Non-test line should be either a '#' or a '# NAME'", iss.eof());
            uassert(9948600,
                    str::stream{} << "testNum must be written in monotonically increasing order. "
                                     "Expected to be at least "
                                  << nextTestNum << ", but got " << localTestNum,
                    localTestNum >= nextTestNum);
            // The first token should be a number. For now ignore and assume it lines
            // up with the number passed in. firstLine.front();
            preQueryComments = readLine(fs, lineFromFile);
            uassert(9670423,
                    str::stream{} << "Expected test line for test #" << localTestNum << " but got "
                                  << lineFromFile,
                    isTestLine(lineFromFile));
            return {lineFromFile, testName};
        }
    }();

    auto expectedResult = std::vector<BSONObj>{};
    auto intraResultSetCommentLineCount = 0;
    if (mode == ModeOption::Normalize) {
        // Read in results set from file.
        for (postQueryComments = readLine(fs, lineFromFile); lineFromFile != "";
             postTestComments = readLine(fs, lineFromFile)) {
            intraResultSetCommentLineCount += postTestComments.size();
            if (lineFromFile.starts_with("[") && lineFromFile.ends_with("]")) {
                // Big performance hit, but this isn't going to be run in the
                // waterfall.
                for (auto&& ele :
                     fromFuzzerJson("{ res : " + lineFromFile + "}").firstElement().Array()) {
                    expectedResult.push_back(ele.Obj().getOwned());
                }
            } else {
                auto sd = StringData{lineFromFile};
                {
                    auto endDocument = sd.rfind("}");
                    if (endDocument == std::string::npos) {
                        continue;
                    }
                    // Count offset of endDocument from the end, but still include the '}'
                    // character.
                    sd.remove_suffix(sd.size() - endDocument - 1);
                }
                {
                    auto startDocument = sd.find("{");
                    if (startDocument == std::string::npos) {
                        continue;
                    }
                    sd.remove_prefix(startDocument);
                }
                if (!sd.empty()) {
                    expectedResult.push_back(fromFuzzerJson(sd));
                }
            }
        }
        uassert(9670406,
                str::stream{} << "Expected results but found none for testNum " << localTestNum,
                !expectedResult.empty());

        if (intraResultSetCommentLineCount > 0) {
            std::cout << applyBrown() << "Warning: we ignored " << intraResultSetCommentLineCount
                      << " lines of intra-result set comments for test "
                      // Print test name or a backspace.
                      << localTestNum << " " << testName.value_or("\b") << "." << applyReset()
                      << std::endl;
        }

        return Test(testLine,
                    optimizationsOff,
                    localTestNum,
                    testName,
                    std::move(preTestComments),
                    std::move(preQueryComments),
                    std::move(postQueryComments),
                    std::move(postTestComments),
                    std::move(expectedResult),
                    overrideOption);
    } else {
        // There is a newline at the end of every test case.
        postQueryComments = readAndAssertNewline(fs, "End of single test without results");
        return Test(testLine,
                    optimizationsOff,
                    localTestNum,
                    testName,
                    std::move(preTestComments),
                    std::move(preQueryComments),
                    std::move(postQueryComments),
                    std::move(postTestComments),
                    {} /* expectedResult */,
                    overrideOption);
    }
}

void Test::parseTestQueryLine() {
    // Override the test type if the override flag was passed.
    if (_overrideOption != OverrideOption::None) {
        _testLine = _testLine.replace(
            0, _testLine.find(' '), overrideOptionToTestTypeString(_overrideOption));
    }
    // First word is test type.
    const auto endTestType = _testLine.find(' ');
    _testType = parseResultType(_testLine.substr(0, endTestType));

    auto queryline = _testLine.substr(endTestType + 1, _testLine.size());

    // If we are running with no optimizations we need to block lowering to find from agg.
    if (_optimizationsOff) {
        const auto containsTextOrGeo =
            pcre::Regex("\\$(text|geoIntersects|geoWithin|near|nearSphere|geoNear|documents)");

        // Some operators require indexes and can't be run with inhibit optimizations.
        if (!containsTextOrGeo.match(queryline)) {
            const auto pipelineStart = pcre::Regex("pipeline\"?[ \t]*:[ \t]*\\[");
            const auto stopOptimization = "{\"$_internalInhibitOptimization\": {}},";
            pipelineStart.substitute(stopOptimization, &queryline, pcre::SUBSTITUTE_GLOBAL);
        }
    }

    // The rest of the string is the query.
    try {
        _query = fromFuzzerJson(queryline);
    } catch (AssertionException& ex) {
        _errorMessage = ex.reason();
        ex.addContext(str::stream{} << "Failed to read test number " << _testNum);
        throw;
    }
}

void Test::runTestAndRecord(DBClientConnection* const conn, const ModeOption mode) {
    try {
        const auto command = shell_utils::isSet(_testType, shell_utils::NormalizationOpts::kExplain)
            ? makeExplainCommand(_query)
            : _query;
        // Populate _normalizedResult so that git diff operates on normalized result sets.
        _normalizedResult = normalize(mode == ModeOption::Normalize
                                          ? _expectedResult
                                          // Run test
                                          : getAllResults(conn, runCommand(conn, _db, command)),
                                      _testType);
    } catch (AssertionException& ex) {
        _errorMessage = ex.reason();
        ex.addContext(str::stream{} << "Error when executing query " << _testNum);
        throw;
    }
}

void Test::setDB(const std::string& db) {
    _db = db;
}

ModeOption stringToModeOption(const std::string& modeString) {
    static const auto kStringToModeOptionMap = std::map<std::string, ModeOption>{
        {"run", ModeOption::Run},
        {"compare", ModeOption::Compare},
        {"normalize", ModeOption::Normalize},
    };

    if (auto itr = kStringToModeOptionMap.find(modeString); itr != kStringToModeOptionMap.end()) {
        return itr->second;
    } else {
        uasserted(9670422, "Only valid options for --mode are 'run', 'compare', and 'normalize'");
    }
}

OverrideOption stringToOverrideOption(const std::string& overrideString) {
    static const auto kStringToOverrideOptionMap = std::map<std::string, OverrideOption>{
        {"queryShapeHash", OverrideOption::QueryShapeHash},
    };

    if (auto itr = kStringToOverrideOptionMap.find(overrideString);
        itr != kStringToOverrideOptionMap.end()) {
        return itr->second;
    }
    uasserted(10387100, "Invalid override option: " + overrideString);
}

boost::optional<std::string> overrideOptionToExtensionPrefix(const OverrideOption overrideOption) {
    switch (overrideOption) {
        case OverrideOption::None:
            return boost::none;
        case OverrideOption::QueryShapeHash:
            return std::string{".queryShapeHash"};
        default:
            MONGO_UNREACHABLE;
    }
}

void Test::writeToStream(std::fstream& fs,
                         const WriteOutOptions resultOpt,
                         const boost::optional<std::string>& errorMsg) const {
    tassert(9670452,
            "Expected file to be open and ready for writing, but it wasn't",
            fs.is_open() && fs.good());
    for (const auto& comment : _comments.preTest) {
        fs << comment << std::endl;
    }
    fs << _testNum;
    if (_testName) {
        fs << " " << _testName.get();
    }
    fs << std::endl;
    for (const auto& comment : _comments.preQuery) {
        fs << comment << std::endl;
    }
    fs << _testLine << std::endl;
    for (const auto& comment : _comments.postQuery) {
        fs << comment << std::endl;
    }

    // If we don't have a normalized result, generate it from the
    // expected result. This is used when writing narrowed results
    // file.
    auto writeOutResult = std::vector<std::string>{};
    const auto& resultRef = [&]() {
        if (_normalizedResult.empty()) {
            for (auto&& bson : _expectedResult) {
                writeOutResult.push_back(bson.jsonString(ExtendedRelaxedV2_0_0, false, false));
            }
            return writeOutResult;
        } else {
            return _normalizedResult;
        }
    }();

    if (errorMsg) {
        fs << errorMsg.get() << std::endl;
    } else {
        // This helps guard against WriteOutOptions that might get added but not handled.
        switch (resultOpt) {
            case WriteOutOptions::kOnelineResult: {
                // Print out just the array.
                fs << LineResult<std::string>{resultRef};
                break;
            }
            case WriteOutOptions::kResult: {
                // Print out each result in the result set on its own line.
                fs << ArrayResult<std::string>{resultRef};
                break;
            }
            default: {
                uasserted(9670436, "Writeout is not supported for that --out argument.");
            }
        }
    }

    for (const auto& comment : _comments.postTest) {
        fs << comment << std::endl;
    }
}
}  // namespace mongo::query_tester
