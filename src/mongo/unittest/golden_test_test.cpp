// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/unittest/golden_test.h"

#include "mongo/unittest/golden_test_base.h"
#include "mongo/unittest/test_info.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <cstdlib>
#include <exception>
#include <ostream>
#include <string>

#include <boost/filesystem.hpp>
#include <fmt/format.h>

namespace mongo::unittest {
namespace {

namespace fs = boost::filesystem;

void setEnvVar(const char* name, const char* value) {
#ifdef _WIN32
    ASSERT_EQUALS(0, _putenv_s(name, value));
#else
    ASSERT_EQUALS(0, setenv(name, value, 1));
#endif
}

void unsetEnvVar(const char* name) {
#ifdef _WIN32
    // Per the CRT contract, setting an environment variable to the empty string deletes it.
    ASSERT_EQUALS(0, _putenv_s(name, ""));
#else
    ASSERT_EQUALS(0, unsetenv(name));
#endif
}

// Sets or clears an environment variable for the duration of its scope and restores its previous
// value afterwards.
class ScopedEnvVar {
public:
    explicit ScopedEnvVar(const char* name, const char* value = nullptr) : _name(name) {
        if (const char* oldValue = std::getenv(name)) {
            _oldValue = oldValue;
        }
        if (value) {
            setEnvVar(name, value);
        } else {
            unsetEnvVar(name);
        }
    }

    ~ScopedEnvVar() {
        if (_oldValue) {
            setEnvVar(_name.c_str(), _oldValue->c_str());
        } else {
            unsetEnvVar(_name.c_str());
        }
    }

    ScopedEnvVar(const ScopedEnvVar&) = delete;
    ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;

private:
    std::string _name;
    boost::optional<std::string> _oldValue;
};

GoldenTestConfig goldenTestConfig{"src/mongo/unittest/expected_output"};

class GoldenSelfTestException : public std::exception {};

// Verify the basic output comparison works.
TEST(GoldenSelfTest, SanityTest) {
    GoldenTestContext ctx(&goldenTestConfig);
    auto& os = ctx.outStream();

    os << "Output 1:\n";
    os << "test test test 1\n";
    os << "Output 2:\n";
    os << "test test\n";
    os << "test 2\n";
}

// Verify the basic output comparison works, when config is reused.
TEST(GoldenSelfTest2, SanityTest2) {
    GoldenTestContext ctx(&goldenTestConfig);
    auto& os = ctx.outStream();
    os << "Output 1:\n";
    os << "test 1\n";
}

// Verify that test path is correctly generated from TestInfo.
TEST(GoldenSelfTest, GoldenTestContextGetPath) {
    GoldenTestContext ctx(&goldenTestConfig);
    ASSERT_EQ(ctx.getTestPath(),
              fs::path("golden_self_test") / fs::path("golden_test_context_get_path.txt"));
}

TEST(GoldenSelfTest, ResolveGoldenDataRootFromConfiguredEnvironmentWins) {
    ScopedEnvVar configPathGuard("GOLDEN_TEST_CONFIG_PATH");
    ScopedEnvVar dataRootGuard("GOLDEN_TEST_DATA_ROOT", "/some/configured/root");
    ScopedEnvVar srcdirGuard("TEST_SRCDIR", "/some/runfiles/root");
    ScopedEnvVar workspaceGuard("TEST_WORKSPACE", "test_workspace");

    auto opts = GoldenTestOptions::parseEnvironment();
    ASSERT(opts.goldenDataRoot);
    ASSERT_EQ(*opts.goldenDataRoot, "/some/configured/root");
    ASSERT_EQ(resolveGoldenDataRoot(opts.goldenDataRoot), fs::path("/some/configured/root"));
}

TEST(GoldenSelfTest, ResolveGoldenDataRootFromBazelTestEnvironment) {
    ScopedEnvVar srcdirGuard("TEST_SRCDIR", "/some/runfiles/root");
    ScopedEnvVar workspaceGuard("TEST_WORKSPACE", "test_workspace");

    ASSERT_EQ(resolveGoldenDataRoot(boost::none),
              fs::path("/some/runfiles/root") / "test_workspace");
}

TEST(GoldenSelfTest, ResolveGoldenDataRootFallsBackToWorkingDirectory) {
    // Temporarily clear the bazel test environment variables to exercise the fallback.
    ScopedEnvVar srcdirGuard("TEST_SRCDIR");
    ScopedEnvVar workspaceGuard("TEST_WORKSPACE");

    ASSERT_EQ(resolveGoldenDataRoot(boost::none), fs::path("."));
}

TEST(GoldenSelfTest, ResolveGoldenDataRootRequiresBothBazelEnvironmentVariables) {
    ScopedEnvVar srcdirGuard("TEST_SRCDIR", "/some/runfiles/root");
    ScopedEnvVar workspaceGuard("TEST_WORKSPACE");
    ASSERT_EQ(resolveGoldenDataRoot(boost::none), fs::path("."));

    unsetEnvVar("TEST_SRCDIR");
    setEnvVar("TEST_WORKSPACE", "test_workspace");
    ASSERT_EQ(resolveGoldenDataRoot(boost::none), fs::path("."));
}

TEST(GoldenSelfTest, ResolveGoldenDataRootIgnoresEmptyConfig) {
    // An empty GOLDEN_TEST_DATA_ROOT (e.g. --test_env inheriting an unset variable) must
    // not resolve to an empty path.
    ASSERT_EQ(resolveGoldenDataRoot(std::string("")), resolveGoldenDataRoot(boost::none));
}

// Verify the basic output comparison works, when config is reused.
TEST(GoldenSelfTest2, DoesNotCompareWhenExceptionThrown) {
    ASSERT_THROWS(
        [&] {
            GoldenTestContext ctx(&goldenTestConfig);
            ctx.outStream() << "No such output" << std::endl;
            throw GoldenSelfTestException();
        }(),
        GoldenSelfTestException);
}
}  // namespace
}  // namespace mongo::unittest
