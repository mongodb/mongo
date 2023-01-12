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

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/iostreams/device/file_descriptor.hpp>
#include <boost/iostreams/stream.hpp>
#include <boost/iostreams/stream_buffer.hpp>
#include <boost/program_options.hpp>
#include <fmt/format.h>
#include <fmt/ostream.h>
#include <yaml-cpp/yaml.h>

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/golden_test_base.h"
#include "mongo/util/ctype.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo::unittest {

namespace fs = ::boost::filesystem;
namespace po = ::boost::program_options;

using namespace fmt::literals;

std::string readFile(const fs::path& path) {
    uassert(6741506, "path must not be a directory", !is_directory(path));
    uassert(6741505, "path must be a regular file", is_regular_file(path));

    std::ostringstream os;
    os << fs::ifstream(path).rdbuf();
    return os.str();
}

void writeFile(const fs::path& path, const std::string& contents) {
    create_directories(path.parent_path());
    fs::ofstream ofs(path);
    ofs << contents;
}

GoldenTestConfig GoldenTestConfig::parseFromBson(const BSONObj& obj) {
    boost::optional<std::string> relativePath;
    for (auto&& elem : obj) {
        if (elem.fieldNameStringData() == "relativePath"_sd) {
            uassert(6741504,
                    "GoldenTestConfig relativePath must be a string",
                    elem.type() == BSONType::String);
            relativePath = elem.String();
        }
    }
    uassert(6741503, "GoldenTestConfig requires a 'relativePath' argument", relativePath);

    return {*relativePath};
}


GoldenTestEnvironment* GoldenTestEnvironment::getInstance() {
    static GoldenTestEnvironment instance;
    return &instance;
}

GoldenTestEnvironment::GoldenTestEnvironment() : _goldenDataRoot(".") {
    // Parse environment variables
    auto opts = GoldenTestOptions::parseEnvironment();

    fs::path outputRoot;
    if (opts.outputRootPattern) {
        fs::path pattern(*opts.outputRootPattern);
        outputRoot = pattern.parent_path() / fs::unique_path(pattern.leaf());
    } else {
        outputRoot = fs::temp_directory_path() / fs::unique_path("out-%%%%-%%%%-%%%%-%%%%");
    }

    _actualOutputRoot = outputRoot / "actual";
    _expectedOutputRoot = outputRoot / "expected";

    if (opts.diffCmd) {
        _diffCmd = *opts.diffCmd;
    } else {
        // Presumably most (all?) platforms we support have git, including git-diff.
        // git-diff also treats missing files as empty, which is convenient.
        _diffCmd = "git diff --no-index \"{{expected}}\" \"{{actual}}\"";
    }
}

std::string GoldenTestEnvironment::diffCmd(const std::string& expectedOutputFile,
                                           const std::string& actualOutputFile) const {
    std::string cmd = _diffCmd;
    auto replace = [&](const std::string& pattern, const std::string& replacement) {
        size_t n = cmd.find(pattern);
        uassert(6741502,
                str::stream() << "diffCmd '" << _diffCmd << "' did not contain '" << pattern << "'",
                n != std::string::npos);
        cmd.replace(n, pattern.size(), replacement);
    };
    replace("{{expected}}", expectedOutputFile);
    replace("{{actual}}", actualOutputFile);
    return cmd;
}

std::string GoldenTestContextBase::toSnakeCase(const std::string& str) {
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

std::string GoldenTestContextBase::sanitizeName(const std::string& str) {
    for (char c : str) {
        bool valid = c == '_' || c == '-' || ctype::isAlnum(c);
        uassert(6741501, "Unsupported character '{}' in name '{}'"_format(c, str), valid);
    }

    return toSnakeCase(str);
}

void GoldenTestContextBase::verifyOutput() {
    std::string actualStr = getOutputString();

    fs::path goldenDataPath = getGoldenDataPath();
    if (!fs::exists(goldenDataPath)) {
        failResultMismatch(
            actualStr, boost::none, "Golden data file doesn't exist: {}"_format(goldenDataPath));
    }

    std::string expectedStr = readFile(goldenDataPath);
    if (actualStr != expectedStr) {
        failResultMismatch(actualStr,
                           expectedStr,
                           "Actual result doesn't match golden data. "
                           "Run 'buildscripts/golden_test.py diff' for more information.");
    }
}

void GoldenTestContextBase::failResultMismatch(const std::string& actualStr,
                                               const boost::optional<std::string>& expectedStr,
                                               const std::string& message) {
    fs::path actualOutputFilePath = getActualOutputPath();
    fs::path expectedOutputFilePath = getExpectedOutputPath();

    writeFile(actualOutputFilePath, actualStr);

    // Write empty expected file even if the expected result was not set.
    // This improves interaction with diff tools that fail or don't show contents if
    // one of the file is missing.
    writeFile(expectedOutputFilePath, expectedStr.get_value_or(""));

    _onError(message, actualStr, expectedStr);
}

fs::path GoldenTestContextBase::getActualOutputPath() const {
    return _env->actualOutputRoot() / _config->relativePath / getTestPath();
}

fs::path GoldenTestContextBase::getExpectedOutputPath() const {
    return _env->expectedOutputRoot() / _config->relativePath / getTestPath();
}

fs::path GoldenTestContextBase::getGoldenDataPath() const {
    return _env->goldenDataRoot() / _config->relativePath / getTestPath();
}

fs::path GoldenTestContextBase::getTestPath() const {
    return _testPath;
}

GoldenTestOptions GoldenTestOptions::parseEnvironment() {
    GoldenTestOptions opts;
    po::options_description desc_env;
    boost::optional<std::string> configPath;
    desc_env.add_options()  //
        ("config_path",
         po::value<std::string>()->notifier([&configPath](auto v) { configPath = v; }));

    po::variables_map vm_env;
    po::store(po::parse_environment(desc_env, "GOLDEN_TEST_"), vm_env);
    po::notify(vm_env);

    if (configPath) {
        std::string configStr = readFile(*configPath);
        YAML::Node configNode = YAML::Load(configStr);

        YAML::Node outputRootPatternNode = configNode["outputRootPattern"];
        if (outputRootPatternNode && outputRootPatternNode.IsScalar()) {
            opts.outputRootPattern = outputRootPatternNode.as<std::string>();
        }

        YAML::Node diffCmdNode = configNode["diffCmd"];
        if (diffCmdNode && diffCmdNode.IsScalar()) {
            opts.diffCmd = diffCmdNode.as<std::string>();
        }
    }

    return opts;
}

}  // namespace mongo::unittest
