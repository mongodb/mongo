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

#include "mongo/base/string_data.h"
#include "mongo/scripting/engine.h"
#include "mongo/scripting/mongo_path_util.h"
#include "mongo/scripting/mozjs/shell/implscope.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <string>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <fmt/format.h>

namespace mongo {
namespace mozjs {

TEST(ModuleLoaderTest, ImportBaseSpecifierFails) {
    mongo::ScriptEngine::setup(ExecutionEnvironment::TestRunner);
    std::unique_ptr<mongo::Scope> scope(mongo::getGlobalScriptEngine()->newScope());

    auto code = "import * as test from \"base_specifier\""_sd;
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

    auto code = "import * as test from \"some_module\""_sd;
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
    auto code = "async function test() { return 42; } await test();"_sd;
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
