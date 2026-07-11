// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/util/modules.h"

#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>

#include <boost/filesystem.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

[[MONGO_MOD_PUBLIC]];

namespace mongo::unittest {

/**
 * Allows executing golden data tests. That is, tests that produce a text output which is compared
 * against checked-in expected results (a.k.a "golden data".)
 *
 * The test fails if its output doesn't match the golden file's contents, or if
 * the golden data file doesn't exist.
 * if expected output doesnt exist. When this happens, the actual and expected outputs will be
 * stored in the configured output location. This allows:
 *  - bulk comparison to determine if further code changes are needed or if the new results are
 * acceptable.
 *  - bulk update of expected outputs if the new outputs are acceptable.
 *
 * Usage:
 *     GoldenTestConfig myConfig("src/mongo/my_expected_output");
 *     TEST(MySuite, MyTest) {
 *         GoldenTestContext ctx(myConfig)
 *         ctx.outStream() << "print something here" << std::endl;
 *         ctx.outStream() << "print something else" << std::endl;
 *     }
 *
 * For complete usage guide and documentation see: docs/golden_data_test_framework.md
 */

/**
 * A configuration specific to each golden test suite.
 */
struct GoldenTestConfig {
    /**
     * A relative path to the golden data files. The path is relative to root of the repo.
     * This path can be shared by multiple suites.
     *
     * It is recommended to keep golden data is a separate subfolder from other source code files.
     * Good:
     *   src/mongo/unittest/expected_output
     *   src/mongo/my_module/my_sub_module/expected_output
     *
     * Bad:
     *   src/mongo/my_module/
     *   src
     *   ../..
     *   /etc
     *   C:\Windows
     */
    std::string relativePath;

    static GoldenTestConfig parseFromBson(const BSONObj&);
};

/**
 * Global environment shared across all golden test suites.
 * Specifically, output directory is shared across all suites to allow simple directory diffing,
 * even if multiple suites were executed.
 */
class GoldenTestEnvironment {
private:
    GoldenTestEnvironment();

public:
    GoldenTestEnvironment(const GoldenTestEnvironment&) = delete;
    GoldenTestEnvironment& operator=(const GoldenTestEnvironment&) = delete;

    static GoldenTestEnvironment* getInstance();

    boost::filesystem::path actualOutputRoot() const {
        return _actualOutputRoot;
    }

    boost::filesystem::path expectedOutputRoot() const {
        return _expectedOutputRoot;
    }

    boost::filesystem::path goldenDataRoot() const {
        return _goldenDataRoot;
    }

    std::string diffCmd(const std::string& expectedOutputFile,
                        const std::string& actualOutputFile) const;

private:
    boost::filesystem::path _goldenDataRoot;
    boost::filesystem::path _actualOutputRoot;
    boost::filesystem::path _expectedOutputRoot;

    std::string _diffCmd;
};

/**
 * Context for each golden test that can be used to accumulate, verify and optionally overwrite test
 * output data. Format of the output data is left to the test implementation. However it is
 * recommended that the output: 1) Is in text format. 2) Can be udated incrementally. Incremental
 * changes to the production or test code should result in incremental changes to the test output.
 * This reduces the size the side of diffs and reduces chances of conflicts. 3) Includes both input
 * and output. This helps with inspecting the changes, without need to pattern match across files.
 */
class [[MONGO_MOD_OPEN]] GoldenTestContextBase {
public:
    explicit GoldenTestContextBase(
        const GoldenTestConfig* config,
        boost::filesystem::path testPath,
        bool validateOnClose,
        std::function<void(const std::string& message,
                           const std::string& actualStr,
                           const boost::optional<std::string>& expectedStr)> onError)
        : _env(GoldenTestEnvironment::getInstance()),
          _config(config),
          _testPath(testPath),
          _validateOnClose(validateOnClose),
          _onError(onError) {}

    ~GoldenTestContextBase() noexcept(false) {
        if (_validateOnClose && !std::uncaught_exceptions()) {
            verifyOutput();
        }
    }

    /**
     * Returns the output stream that a test should write its output to.
     * The output that is written here will be compared against expected golden data. If the output
     * that test produced differs from the expected output, the test will fail.
     */
    std::ostream& outStream() {
        return _outStream;
    }

    /**
     * Verifies that output accumulated in this context matches the expected output golden data.
     * If output does not match, the test fails with TestAssertionFailureException.
     *
     * Additionally, in case of mismatch:
     *  - a file with the actual test output is created.
     *  - a file with the expected output is created:
     *    this preserves the snapshot of the golden data that was used for verification, as the
     * files in the source repo can subsequently change. Output files are written to a temp files
     * location unless configured otherwise.
     */
    void verifyOutput();

    /**
     * Returns the path where the actual test output will be written.
     */
    boost::filesystem::path getActualOutputPath() const;

    /**
     * Returns the path where the expected test output will be written.
     */
    boost::filesystem::path getExpectedOutputPath() const;

    /**
     * Returns the path to the golden data used for verification.
     */
    boost::filesystem::path getGoldenDataPath() const;

    /**
     * Returns relative test path. Typically composed of suite and test names.
     */
    boost::filesystem::path getTestPath() const;

    /**
     * Returns the output accumulated in the stream.
     */
    std::string getOutputString() const {
        return _outStream.str();
    }

    /**
     * Sets whether the context should verify the output in the destructor.
     */
    bool validateOnClose() const {
        return _validateOnClose;
    }

    /**
     * Returns whether the context should verify the output in the destrutor.
     */
    void validateOnClose(bool value) {
        _validateOnClose = value;
    }

protected:
    static std::string sanitizeName(const std::string& str);
    const GoldenTestEnvironment* getEnv() const {
        return _env;
    }

private:
    static std::string toSnakeCase(const std::string& str);
    static boost::filesystem::path toTestPath(const std::string& suiteName,
                                              const std::string& testName);

    void failResultMismatch(const std::string& actualStr,
                            const boost::optional<std::string>& expectedStr,
                            const std::string& messsage);

    GoldenTestEnvironment* _env;
    const GoldenTestConfig* _config;
    const boost::filesystem::path _testPath;
    bool _validateOnClose;
    std::ostringstream _outStream;
    // Use a callback instead of a virtual function, because calling virtual functions doesn't work
    // properly in a destructor, even a virtual destructor.
    const std::function<void(const std::string& message,
                             const std::string& actualStr,
                             const boost::optional<std::string>& expectedStr)>
        _onError;
};

/**
 * Represents configuration variables used by golden tests.
 */
struct GoldenTestOptions {
    /**
     * Parses the options from environment variables that start with GOLDEN_TEST_ prefix.
     * Supported options:
     *  - GOLDEN_TEST_CONFIG_PATH: (optional) specifies the yaml config file.
     *    See config file reference:
     * docs/golden_data_test_framework.md#appendix---config-file-reference
     *  - GOLDEN_TEST_OUTPUT_ROOT_PATTERN: (optional) overrides the `outputRootPattern` value
     *    loaded from the yaml config. Intended to let resmoke.py pre-resolve a single output
     *    root and share it across all golden tests in one invocation.
     */
    static GoldenTestOptions parseEnvironment();

    /**
     * Root path patten that will be used to write expected and actual test outputs for all tests in
     * the test run. If not specified a temporary folder location will be used. Path pattern string
     * may use '%' characters in the last part of the path. '%' characters will be replaced with
     * random lowercase hexadecimal digits.
     */
    boost::optional<std::string> outputRootPattern;

    /**
     * Shell command to diff a single golden test run output.
     * {{expected}} and {{actual}} variables should be used and will be replaced  with expected and
     * actual output paths respectively.
     */
    boost::optional<std::string> diffCmd;
};

}  // namespace mongo::unittest
