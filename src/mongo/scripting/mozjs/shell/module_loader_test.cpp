// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/scripting/engine.h"
#include "mongo/scripting/mongo_path_util.h"
#include "mongo/scripting/mozjs/shell/implscope.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <string>
#include <string_view>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <fmt/format.h>

namespace mongo {
namespace mozjs {
using namespace std::literals::string_view_literals;

TEST(ModuleLoaderTest, ImportBaseSpecifierFails) {
    mongo::ScriptEngine::setup(ExecutionEnvironment::TestRunner);
    std::unique_ptr<mongo::Scope> scope(mongo::getGlobalScriptEngine()->newScope());

    auto code = "import * as test from \"base_specifier\""sv;
    ASSERT_THROWS_WITH_CHECK(
        scope->exec(code,
                    "root_module",
                    true /* printResult */,
                    true /* reportError */,
                    true /* assertOnError , timeout*/),
        DBException,
        [&](const auto& ex) { ASSERT_STRING_CONTAINS(ex.what(), "Cannot find module"); });
    scope.reset();
    setGlobalScriptEngine(nullptr);
}

#if !defined(_WIN32)
TEST(ModuleLoaderTest, ImportDirectoryFails) {
    mongo::ScriptEngine::setup(ExecutionEnvironment::TestRunner);
    std::unique_ptr<mongo::Scope> scope(mongo::getGlobalScriptEngine()->newScope());

    auto code = fmt::format("import * as test from \"{}\"",
                            boost::filesystem::temp_directory_path().string());
    ASSERT_THROWS_WITH_CHECK(
        scope->exec(code,
                    "root_module",
                    true /* printResult */,
                    true /* reportError */,
                    true /* assertOnError , timeout*/),
        DBException,
        [&](const auto& ex) { ASSERT_STRING_CONTAINS(ex.what(), "Directory import"); });
    scope.reset();
    setGlobalScriptEngine(nullptr);
}
#endif

TEST(ModuleLoaderTest, ImportInInteractiveFails) {
    mongo::ScriptEngine::setup(ExecutionEnvironment::TestRunner);
    std::unique_ptr<mongo::Scope> scope(mongo::getGlobalScriptEngine()->newScope());

    auto code = "import * as test from \"some_module\""sv;
    ASSERT_THROWS_WITH_CHECK(
        scope->exec(code,
                    MozJSImplScope::kInteractiveShellName,
                    true /* printResult */,
                    true /* reportError */,
                    true /* assertOnError , timeout*/),
        DBException,
        [&](const auto& ex) {
            ASSERT_STRING_CONTAINS(ex.what(),
                                   "import declarations may only appear at top level of a module");
        });
    scope.reset();
    setGlobalScriptEngine(nullptr);
}

TEST(ModuleLoaderTest, TopLevelAwaitWorks) {
    mongo::ScriptEngine::setup(ExecutionEnvironment::TestRunner);
    std::unique_ptr<mongo::Scope> scope(mongo::getGlobalScriptEngine()->newScope());
    auto code = "async function test() { return 42; } await test();"sv;
    ASSERT_DOES_NOT_THROW(scope->exec(code,
                                      "root_module",
                                      true /* printResult */,
                                      true /* reportError */,
                                      true /* assertOnError , timeout*/));
    scope.reset();
    setGlobalScriptEngine(nullptr);
}

TEST(ModuleLoaderTest, ParseSearchPathsEmpty) {
    // Test with no MONGO_PATH set - should default to current working directory
#ifdef _WIN32
    _putenv_s("MONGO_PATH", "");
#else
    unsetenv("MONGO_PATH");
#endif
    auto paths = parseMongoPath();
    ASSERT_EQ(paths.size(), 1U);
    ASSERT_EQ(paths[0], boost::filesystem::current_path().string());
}

TEST(ModuleLoaderTest, ParseSearchPathsSinglePath) {
    // Test with a single path
#ifdef _WIN32
    _putenv_s("MONGO_PATH", "C:\\mongo\\lib");
    auto paths = parseMongoPath();
    ASSERT_EQ(paths.size(), 1U);
    ASSERT_EQ(paths[0], "C:\\mongo\\lib");
    _putenv_s("MONGO_PATH", "");
#else
    setenv("MONGO_PATH", "/usr/local/lib/mongo", 1);
    auto paths = parseMongoPath();
    ASSERT_EQ(paths.size(), 1U);
    ASSERT_EQ(paths[0], "/usr/local/lib/mongo");
    unsetenv("MONGO_PATH");
#endif
}

#if !defined(_WIN32)
TEST(ModuleLoaderTest, ParseSearchPathsMultiplePathsUnix) {
    // Test with multiple paths (Unix-style colon separator)
    setenv("MONGO_PATH", "/usr/local/lib/mongo:/opt/mongo/lib:/home/user/.mongo", 1);
    auto paths = parseMongoPath();
    ASSERT_EQ(paths.size(), 3U);
    ASSERT_EQ(paths[0], "/usr/local/lib/mongo");
    ASSERT_EQ(paths[1], "/opt/mongo/lib");
    ASSERT_EQ(paths[2], "/home/user/.mongo");
    unsetenv("MONGO_PATH");
}
#else
TEST(ModuleLoaderTest, ParseSearchPathsMultiplePathsWindows) {
    // Test with multiple paths (Windows-style semicolon separator)
    _putenv_s("MONGO_PATH", "C:\\mongo\\lib;D:\\mongo\\lib");
    auto paths = parseMongoPath();
    ASSERT_EQ(paths.size(), 2U);
    ASSERT_EQ(paths[0], "C:\\mongo\\lib");
    ASSERT_EQ(paths[1], "D:\\mongo\\lib");
    _putenv_s("MONGO_PATH", "");
}
#endif

TEST(ModuleLoaderTest, ParseSearchPathsIgnoresEmpty) {
    // Test that empty paths are ignored
#ifdef _WIN32
    _putenv_s("MONGO_PATH", "C:\\mongo\\lib;;D:\\mongo\\lib");
    auto paths = parseMongoPath();
    ASSERT_EQ(paths.size(), 2U);
    ASSERT_EQ(paths[0], "C:\\mongo\\lib");
    ASSERT_EQ(paths[1], "D:\\mongo\\lib");
    _putenv_s("MONGO_PATH", "");
#else
    setenv("MONGO_PATH", "/usr/local/lib/mongo::/opt/mongo/lib", 1);
    auto paths = parseMongoPath();
    ASSERT_EQ(paths.size(), 2U);
    ASSERT_EQ(paths[0], "/usr/local/lib/mongo");
    ASSERT_EQ(paths[1], "/opt/mongo/lib");
    unsetenv("MONGO_PATH");
#endif
}

}  // namespace mozjs
}  // namespace mongo
