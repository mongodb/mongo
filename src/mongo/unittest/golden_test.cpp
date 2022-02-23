/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include <boost/filesystem.hpp>
#include <boost/iostreams/device/file_descriptor.hpp>
#include <boost/iostreams/stream.hpp>
#include <boost/iostreams/stream_buffer.hpp>
#include <boost/lexical_cast.hpp>
#include <fmt/format.h>
#include <fmt/ostream.h>
#include <pcrecpp.h>

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/golden_test.h"
#include "mongo/util/ctype.h"
#include "mongo/util/options_parser/option_section.h"
#include "mongo/util/options_parser/startup_option_init.h"
#include "mongo/util/options_parser/startup_options.h"

namespace mongo::unittest {

namespace fs = ::boost::filesystem;
using namespace fmt::literals;

static const pcrecpp::RE validNameRegex(R"([[:alnum:]_\-]*)");

GoldenTestEnvironment* GoldenTestEnvironment::getInstance() {
    static GoldenTestEnvironment instance;
    return &instance;
}

GoldenTestEnvironment::GoldenTestEnvironment()
    : _goldenDataRoot("."), _outputPathPrefix("test_output") {
    // Parse environment variables
    auto opts = GoldenTestOptions::parseEnvironment();

    fs::path outputRoot;
    if (opts.output) {
        outputRoot = fs::path(opts.output.get());
    } else {
        fs::path tempRoot = fs::temp_directory_path();
        fs::path uniqueDir = fs::unique_path(_outputPathPrefix + "-%%%%-%%%%-%%%%-%%%%");
        outputRoot = tempRoot / uniqueDir;
    }

    _actualOutputRoot = outputRoot / "actual";
    _expectedOutputRoot = outputRoot / "expected";
}

std::string GoldenTestContext::toSnakeCase(const std::string& str) {
    std::string result;
    bool lastAlpha = false;
    for (char c : str) {
        if (ctype::isUpper(c)) {
            if (lastAlpha) {
                result += '_';
            }

            result += ctype::toLower(c);
        } else {
            result += c;
        }

        lastAlpha = ctype::isAlpha(c);
    }

    return result;
}

std::string GoldenTestContext::sanitizeName(const std::string& str) {
    if (!validNameRegex.FullMatch(str)) {
        FAIL("Unsupported characters in name '{}'"_format(str));
    }

    return toSnakeCase(str);
}

void GoldenTestContext::verifyOutput() {
    std::string actualStr = _outStream.str();

    fs::path goldenDataPath = getGoldedDataPath();
    if (!fs::exists(goldenDataPath)) {
        failResultMismatch(actualStr, boost::none, "Golden data file doesn't exist.");
    }

    std::string expectedStr = readFile(goldenDataPath);
    if (actualStr != expectedStr) {
        failResultMismatch(actualStr, expectedStr, "Actual result doesn't match golden data.");
    }
}

void GoldenTestContext::failResultMismatch(const std::string& actualStr,
                                           const boost::optional<std::string>& expectedStr,
                                           const std::string& message) {
    fs::path actualOutputFilePath = getActualOutputPath();
    fs::path expectedOutputFilePath = getExpectedOutputPath();

    writeFile(actualOutputFilePath, actualStr);
    if (expectedStr != boost::none) {
        writeFile(expectedOutputFilePath, *expectedStr);
    }

    LOGV2_ERROR(6273501,
                "Test output verification failed",
                "message"_attr = message,
                "actualOutput"_attr = actualStr,
                "expectedOutput"_attr = expectedStr,
                "actualOutputPath"_attr = actualOutputFilePath.string(),
                "expectedOutputPath"_attr = expectedOutputFilePath.string(),
                "actualOutputRoot"_attr = _env->actualOutputRoot().string(),
                "expectedOutputRoot"_attr = _env->expectedOutputRoot().string());

    throwAssertionFailureException(
        "Test output verification failed: {}, "
        "actual output file: {}, "
        "expected output file: {}"
        ""_format(message, actualOutputFilePath, expectedOutputFilePath));
}

std::string GoldenTestContext::readFile(const fs::path& path) {
    ASSERT_FALSE(is_directory(path));
    ASSERT_TRUE(is_regular_file(path));

    std::ostringstream os;
    os << fs::ifstream(path).rdbuf();
    return os.str();
}

void GoldenTestContext::writeFile(const fs::path& path, const std::string& contents) {
    create_directories(path.parent_path());
    fs::ofstream ofs(path);
    ofs << contents;
}

void GoldenTestContext::throwAssertionFailureException(const std::string& message) {
    throw TestAssertionFailureException(_testInfo->file().toString(), _testInfo->line(), message);
}

fs::path GoldenTestContext::getActualOutputPath() const {
    return _env->actualOutputRoot() / _config->relativePath / getTestPath();
}

fs::path GoldenTestContext::getExpectedOutputPath() const {
    return _env->expectedOutputRoot() / _config->relativePath / getTestPath();
}

fs::path GoldenTestContext::getGoldedDataPath() const {
    return _env->goldenDataRoot() / _config->relativePath / getTestPath();
}

fs::path GoldenTestContext::getTestPath() const {
    return fs::path(sanitizeName(_testInfo->suiteName().toString())) /
        fs::path(sanitizeName(_testInfo->testName().toString()) + ".txt");
}

GoldenTestOptions GoldenTestOptions::parseEnvironment() {
    GoldenTestOptions opts;
    namespace po = ::boost::program_options;
    po::options_description desc_env;
    desc_env.add_options()  //
        ("output", po::value<std::string>()->notifier([&opts](auto v) { opts.output = v; }));

    po::variables_map vm_env;
    po::store(po::parse_environment(desc_env, "GOLDEN_TEST_"), vm_env);
    po::notify(vm_env);

    return opts;
}

}  // namespace mongo::unittest
