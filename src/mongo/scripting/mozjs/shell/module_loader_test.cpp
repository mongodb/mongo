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

#include "mongo/scripting/mozjs/shell/module_loader.h"

#include "mongo/base/string_data.h"
#include "mongo/scripting/engine.h"
#include "mongo/scripting/mongo_path_util.h"
#include "mongo/scripting/mozjs/shell/implscope.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"

#include <fstream>
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

TEST(ModuleLoaderTest, ModulesNotSupportedInServerEnvironment) {
    mongo::ScriptEngine::setup(ExecutionEnvironment::Server);
    {
        std::unique_ptr<mongo::Scope> scope(
            mongo::getGlobalScriptEngine()->newScopeForCurrentThread());
        auto* implscope = dynamic_cast<mongo::mozjs::MozJSImplScope*>(scope.get());
        ASSERT_TRUE(implscope != nullptr);

        // Module loading is not supported in the server execution environment: the module loader is
        // never constructed (so getModuleLoader() must not be called) and no filesystem-backed
        // module hooks are registered. This is what prevents server-side JavaScript from reading
        // host files via import().
        ASSERT_FALSE(implscope->supportsModules());
    }
    setGlobalScriptEngine(nullptr);
}

TEST(ModuleLoaderTest, ModulesSupportedInTestRunnerEnvironment) {
    mongo::ScriptEngine::setup(ExecutionEnvironment::TestRunner);
    {
        std::unique_ptr<mongo::Scope> scope(
            mongo::getGlobalScriptEngine()->newScopeForCurrentThread());
        auto* implscope = dynamic_cast<mongo::mozjs::MozJSImplScope*>(scope.get());
        ASSERT_TRUE(implscope != nullptr);

        ASSERT_TRUE(implscope->supportsModules());
    }
    setGlobalScriptEngine(nullptr);
}

// These negative cases trip a tassert. Caught tasserts leave the tripwire counter set, which aborts
// the binary at process exit, so they are written as DEATH_TESTs (MongoDB's convention for testing
// tasserts) rather than ASSERT_THROWS_CODE catches.
DEATH_TEST(ModuleLoaderDeathTest, GetModuleLoaderInServerEnvironment, "12883200") {
    mongo::ScriptEngine::setup(ExecutionEnvironment::Server);
    std::unique_ptr<mongo::Scope> scope(mongo::getGlobalScriptEngine()->newScopeForCurrentThread());
    auto* implscope = dynamic_cast<mongo::mozjs::MozJSImplScope*>(scope.get());
    ASSERT_TRUE(implscope != nullptr);

    // There is no module loader in the server environment, so reaching for it tasserts.
    implscope->getModuleLoader();
}

DEATH_TEST(ModuleLoaderDeathTest, GetBaseURLInServerEnvironment, "12883201") {
    mongo::ScriptEngine::setup(ExecutionEnvironment::Server);
    std::unique_ptr<mongo::Scope> scope(mongo::getGlobalScriptEngine()->newScopeForCurrentThread());
    auto* implscope = dynamic_cast<mongo::mozjs::MozJSImplScope*>(scope.get());
    ASSERT_TRUE(implscope != nullptr);

    // getBaseURL() defers to the module loader, so it tasserts on the server as well.
    implscope->getBaseURL();
}

DEATH_TEST(ModuleLoaderDeathTest, ConstructInServerEnvironment, "12883202") {
    // The ModuleLoader constructor itself refuses the server execution environment, as a backstop
    // against any caller that bypasses MozJSImplScope's gating.
    [[maybe_unused]] ModuleLoader loader(ExecutionEnvironment::Server);
}

TEST(ModuleLoaderTest, ImportDisabledInServerEnvironment) {
    mongo::ScriptEngine::setup(ExecutionEnvironment::Server);
    std::unique_ptr<mongo::Scope> scope(mongo::getGlobalScriptEngine()->newScope());

    // Write a real file on disk that a malicious import would try to read (e.g. a keyFile or PEM).
    static constexpr auto kSecret = "TOP_SECRET_FILE_CONTENTS_e3b0c44298"_sd;
    auto modulePath = boost::filesystem::temp_directory_path() / "server_import_disabled_test.js";
    {
        std::ofstream out(modulePath.string());
        out << "export const secret = '" << kSecret << "';\n";
    }
    ON_BLOCK_EXIT([&] { boost::filesystem::remove(modulePath); });

    // On the server, module loading is not supported: a failed classic-script compilation is never
    // retried as a module, so the import is never resolved and the file is never read. The original
    // SyntaxError propagates instead. The scope itself was constructed successfully above, proving
    // the self-contained server setup scripts load without the module loader.
    auto code = fmt::format("import * as test from \"{}\"", modulePath.generic_string());
    ASSERT_THROWS_WITH_CHECK(
        scope->exec(StringData(code),
                    "root_module",
                    true /* printResult */,
                    true /* reportError */,
                    true /* assertOnError , timeout*/),
        DBException,
        [&](const auto& ex) {
            ASSERT_STRING_CONTAINS(ex.what(),
                                   "import declarations may only appear at top level of a module");
            // The file contents must never have been read into the error or anywhere else.
            ASSERT_EQ(std::string(ex.what()).find(std::string{kSecret}), std::string::npos);
        });
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
