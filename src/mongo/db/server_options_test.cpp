/* Copyright 2013 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/config.h"

#if defined(MONGO_CONFIG_HAVE_HEADER_UNISTD_H)
#include <unistd.h>
#endif
#ifndef _WIN32
#include <sys/types.h>
#include <sys/wait.h>
#endif

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#include <boost/filesystem.hpp>

#include "mongo/bson/util/builder.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_options_helpers.h"
#include "mongo/logger/logger.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/log.h"
#include "mongo/util/options_parser/environment.h"
#include "mongo/util/options_parser/option_section.h"
#include "mongo/util/options_parser/options_parser.h"
#include "mongo/util/scopeguard.h"

namespace {

using mongo::ErrorCodes;
using mongo::Status;

namespace moe = mongo::optionenvironment;

class OptionsParserTester : public moe::OptionsParser {
public:
    Status readConfigFile(const std::string& filename, std::string* config) {
        if (filename != _filename) {
            ::mongo::StringBuilder sb;
            sb << "Parser using filename: " << filename
               << " which does not match expected filename: " << _filename;
            return Status(ErrorCodes::InternalError, sb.str());
        }
        *config = _config;
        return Status::OK();
    }
    void setConfig(const std::string& filename, const std::string& config) {
        _filename = filename;
        _config = config;
    }

private:
    std::string _filename;
    std::string _config;
};

TEST(Verbosity, Default) {
    OptionsParserTester parser;
    moe::Environment environment;
    moe::OptionSection options;

    // Reset the log level before we test
    ::mongo::logger::globalLogDomain()->setMinimumLoggedSeverity(
        ::mongo::logger::LogSeverity::Info());

    ASSERT_OK(::mongo::addGeneralServerOptions(&options));

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    std::map<std::string, std::string> env_map;

    ASSERT_OK(parser.run(options, argv, env_map, &environment));

    ASSERT_OK(::mongo::validateServerOptions(environment));
    ASSERT_OK(::mongo::canonicalizeServerOptions(&environment));
    ASSERT_OK(::mongo::setupServerOptions(argv));
    ASSERT_OK(::mongo::storeServerOptions(environment));

    // Make sure the log level didn't change since we didn't specify any verbose options
    ASSERT_EQUALS(::mongo::logger::globalLogDomain()->getMinimumLogSeverity(),
                  ::mongo::logger::LogSeverity::Info());
}

TEST(Verbosity, CommandLineImplicit) {
    OptionsParserTester parser;
    moe::Environment environment;
    moe::OptionSection options;

    // Reset the log level before we test
    ::mongo::logger::globalLogDomain()->setMinimumLoggedSeverity(
        ::mongo::logger::LogSeverity::Info());

    ASSERT_OK(::mongo::addGeneralServerOptions(&options));

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--verbose");
    std::map<std::string, std::string> env_map;

    ASSERT_OK(parser.run(options, argv, env_map, &environment));

    ASSERT_OK(::mongo::validateServerOptions(environment));
    ASSERT_OK(::mongo::canonicalizeServerOptions(&environment));
    ASSERT_OK(::mongo::setupServerOptions(argv));
    ASSERT_OK(::mongo::storeServerOptions(environment));

    int verbosity = 1;
    ASSERT_EQUALS(::mongo::logger::globalLogDomain()->getMinimumLogSeverity(),
                  ::mongo::logger::LogSeverity::Debug(verbosity));
}

TEST(Verbosity, CommandLineString) {
    OptionsParserTester parser;
    moe::Environment environment;
    moe::OptionSection options;

    // Reset the log level before we test
    ::mongo::logger::globalLogDomain()->setMinimumLoggedSeverity(
        ::mongo::logger::LogSeverity::Info());

    ASSERT_OK(::mongo::addGeneralServerOptions(&options));

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--verbose");
    argv.push_back("vvvv");
    std::map<std::string, std::string> env_map;

    ASSERT_OK(parser.run(options, argv, env_map, &environment));

    ASSERT_OK(::mongo::validateServerOptions(environment));
    ASSERT_OK(::mongo::canonicalizeServerOptions(&environment));
    ASSERT_OK(::mongo::setupServerOptions(argv));
    ASSERT_OK(::mongo::storeServerOptions(environment));

    int verbosity = 4;
    ASSERT_EQUALS(::mongo::logger::globalLogDomain()->getMinimumLogSeverity(),
                  ::mongo::logger::LogSeverity::Debug(verbosity));
}

TEST(Verbosity, CommandLineStringDisguisedLongForm) {
    OptionsParserTester parser;
    moe::Environment environment;
    moe::OptionSection options;

    // Reset the log level before we test
    ::mongo::logger::globalLogDomain()->setMinimumLoggedSeverity(
        ::mongo::logger::LogSeverity::Info());

    ASSERT_OK(::mongo::addGeneralServerOptions(&options));

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("-verbose");
    argv.push_back("vvvv");
    std::map<std::string, std::string> env_map;

    ASSERT_OK(parser.run(options, argv, env_map, &environment));

    ASSERT_OK(::mongo::validateServerOptions(environment));
    ASSERT_OK(::mongo::canonicalizeServerOptions(&environment));
    ASSERT_OK(::mongo::setupServerOptions(argv));
    ASSERT_OK(::mongo::storeServerOptions(environment));

    int verbosity = 4;
    ASSERT_EQUALS(::mongo::logger::globalLogDomain()->getMinimumLogSeverity(),
                  ::mongo::logger::LogSeverity::Debug(verbosity));
}

TEST(Verbosity, CommandLineEmptyString) {
    OptionsParserTester parser;
    moe::Environment environment;
    moe::OptionSection options;

    // Reset the log level before we test
    ::mongo::logger::globalLogDomain()->setMinimumLoggedSeverity(
        ::mongo::logger::LogSeverity::Info());

    ASSERT_OK(::mongo::addGeneralServerOptions(&options));

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--verbose");
    argv.push_back("");
    std::map<std::string, std::string> env_map;

    ASSERT_NOT_OK(parser.run(options, argv, env_map, &environment));
}

TEST(Verbosity, CommandLineBadString) {
    OptionsParserTester parser;
    moe::Environment environment;
    moe::OptionSection options;

    // Reset the log level before we test
    ::mongo::logger::globalLogDomain()->setMinimumLoggedSeverity(
        ::mongo::logger::LogSeverity::Info());

    ASSERT_OK(::mongo::addGeneralServerOptions(&options));

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--verbose");
    argv.push_back("beloud");
    std::map<std::string, std::string> env_map;

    ASSERT_OK(parser.run(options, argv, env_map, &environment));

    ASSERT_NOT_OK(::mongo::validateServerOptions(environment));
}

TEST(Verbosity, CommandLineBadStringOnlyDash) {
    OptionsParserTester parser;
    moe::Environment environment;
    moe::OptionSection options;

    // Reset the log level before we test
    ::mongo::logger::globalLogDomain()->setMinimumLoggedSeverity(
        ::mongo::logger::LogSeverity::Info());

    ASSERT_OK(::mongo::addGeneralServerOptions(&options));

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("-");
    std::map<std::string, std::string> env_map;

    ASSERT_NOT_OK(parser.run(options, argv, env_map, &environment));
}

TEST(Verbosity, CommandLineBadStringOnlyTwoDashes) {
    OptionsParserTester parser;
    moe::Environment environment;
    moe::OptionSection options;

    // Reset the log level before we test
    ::mongo::logger::globalLogDomain()->setMinimumLoggedSeverity(
        ::mongo::logger::LogSeverity::Info());

    ASSERT_OK(::mongo::addGeneralServerOptions(&options));

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--");
    std::map<std::string, std::string> env_map;

    ASSERT_OK(parser.run(options, argv, env_map, &environment));
}

TEST(Verbosity, INIConfigString) {
    OptionsParserTester parser;
    moe::Environment environment;
    moe::OptionSection options;

    // Reset the log level before we test
    ::mongo::logger::globalLogDomain()->setMinimumLoggedSeverity(
        ::mongo::logger::LogSeverity::Info());

    ASSERT_OK(::mongo::addGeneralServerOptions(&options));

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.ini");
    std::map<std::string, std::string> env_map;

    parser.setConfig("config.ini", "verbose=vvvv");

    ASSERT_OK(parser.run(options, argv, env_map, &environment));

    ASSERT_OK(::mongo::validateServerOptions(environment));
    ASSERT_OK(::mongo::canonicalizeServerOptions(&environment));
    ASSERT_OK(::mongo::setupServerOptions(argv));
    ASSERT_OK(::mongo::storeServerOptions(environment));

    int verbosity = 4;
    ASSERT_EQUALS(::mongo::logger::globalLogDomain()->getMinimumLogSeverity(),
                  ::mongo::logger::LogSeverity::Debug(verbosity));
}

TEST(Verbosity, INIConfigBadString) {
    OptionsParserTester parser;
    moe::Environment environment;
    moe::OptionSection options;

    // Reset the log level before we test
    ::mongo::logger::globalLogDomain()->setMinimumLoggedSeverity(
        ::mongo::logger::LogSeverity::Info());

    ASSERT_OK(::mongo::addGeneralServerOptions(&options));

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.ini");
    std::map<std::string, std::string> env_map;

    parser.setConfig("config.ini", "verbose=beloud");

    ASSERT_OK(parser.run(options, argv, env_map, &environment));

    ASSERT_NOT_OK(::mongo::validateServerOptions(environment));
}

TEST(Verbosity, INIConfigEmptyString) {
    OptionsParserTester parser;
    moe::Environment environment;
    moe::OptionSection options;

    // Reset the log level before we test
    ::mongo::logger::globalLogDomain()->setMinimumLoggedSeverity(
        ::mongo::logger::LogSeverity::Info());

    ASSERT_OK(::mongo::addGeneralServerOptions(&options));

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.ini");
    std::map<std::string, std::string> env_map;

    parser.setConfig("config.ini", "verbose=");

    ASSERT_OK(parser.run(options, argv, env_map, &environment));

    ASSERT_OK(::mongo::validateServerOptions(environment));
    ASSERT_OK(::mongo::canonicalizeServerOptions(&environment));
    ASSERT_OK(::mongo::setupServerOptions(argv));
    ASSERT_OK(::mongo::storeServerOptions(environment));

    int verbosity = 0;
    ASSERT_EQUALS(::mongo::logger::globalLogDomain()->getMinimumLogSeverity(),
                  ::mongo::logger::LogSeverity::Debug(verbosity));
}

TEST(Verbosity, JSONConfigString) {
    OptionsParserTester parser;
    moe::Environment environment;
    moe::OptionSection options;

    // Reset the log level before we test
    ::mongo::logger::globalLogDomain()->setMinimumLoggedSeverity(
        ::mongo::logger::LogSeverity::Info());

    ASSERT_OK(::mongo::addGeneralServerOptions(&options));

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.json");
    std::map<std::string, std::string> env_map;

    parser.setConfig("config.json", "{ \"systemLog.verbosity\" : 4 }");

    ASSERT_OK(parser.run(options, argv, env_map, &environment));

    ASSERT_OK(::mongo::validateServerOptions(environment));
    ASSERT_OK(::mongo::canonicalizeServerOptions(&environment));
    ASSERT_OK(::mongo::setupServerOptions(argv));
    ASSERT_OK(::mongo::storeServerOptions(environment));

    int verbosity = 4;
    ASSERT_EQUALS(::mongo::logger::globalLogDomain()->getMinimumLogSeverity(),
                  ::mongo::logger::LogSeverity::Debug(verbosity));
}

TEST(Verbosity, MultipleSourcesMultipleOptions) {
    OptionsParserTester parser;
    moe::Environment environment;
    moe::OptionSection options;

    // Reset the log level before we test
    ::mongo::logger::globalLogDomain()->setMinimumLoggedSeverity(
        ::mongo::logger::LogSeverity::Info());

    ASSERT_OK(::mongo::addGeneralServerOptions(&options));

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.json");
    argv.push_back("--verbose");
    argv.push_back("vvv");
    std::map<std::string, std::string> env_map;

    parser.setConfig("config.json", "{ \"systemLog.verbosity\" : 4 }");

    ASSERT_OK(parser.run(options, argv, env_map, &environment));

    ASSERT_OK(::mongo::validateServerOptions(environment));
    ASSERT_OK(::mongo::canonicalizeServerOptions(&environment));
    ASSERT_OK(::mongo::setupServerOptions(argv));
    ASSERT_OK(::mongo::storeServerOptions(environment));

    int verbosity = 3;
    ASSERT_EQUALS(::mongo::logger::globalLogDomain()->getMinimumLogSeverity(),
                  ::mongo::logger::LogSeverity::Debug(verbosity));
}

TEST(Verbosity, YAMLConfigStringLogComponent) {
    OptionsParserTester parser;
    moe::Environment environment;
    moe::OptionSection options;

    // Reset the log level before we test
    ::mongo::logger::globalLogDomain()->setMinimumLoggedSeverity(
        ::mongo::logger::LogSeverity::Info());
    // Log level for Storage will be cleared by config file value.
    ::mongo::logger::globalLogDomain()->setMinimumLoggedSeverity(
        ::mongo::logger::LogComponent::kStorage, ::mongo::logger::LogSeverity::Debug(1));

    ASSERT_OK(::mongo::addGeneralServerOptions(&options));

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.yaml");
    std::map<std::string, std::string> env_map;

    parser.setConfig("config.yaml",
                     "systemLog:\n"
                     "    verbosity: 4\n"
                     "    component:\n"
                     "        accessControl:\n"
                     "            verbosity: 0\n"
                     "        storage:\n"
                     "            verbosity: -1\n"
                     "            journal:\n"
                     "                verbosity: 2\n");

    ASSERT_OK(parser.run(options, argv, env_map, &environment));

    ASSERT_OK(::mongo::validateServerOptions(environment));
    ASSERT_OK(::mongo::canonicalizeServerOptions(&environment));
    ASSERT_OK(::mongo::setupServerOptions(argv));
    ASSERT_OK(::mongo::storeServerOptions(environment));

    // Verify component log levels using global log domain.
    int verbosity = 4;

    // Default
    ASSERT_EQUALS(::mongo::logger::globalLogDomain()->getMinimumLogSeverity(),
                  ::mongo::logger::LogSeverity::Debug(verbosity));
    ASSERT_EQUALS(::mongo::logger::globalLogDomain()->getMinimumLogSeverity(
                      ::mongo::logger::LogComponent::kDefault),
                  ::mongo::logger::LogSeverity::Debug(verbosity));

    // AccessControl
    ASSERT_TRUE(::mongo::logger::globalLogDomain()->hasMinimumLogSeverity(
        ::mongo::logger::LogComponent::kAccessControl));
    ASSERT_EQUALS(::mongo::logger::globalLogDomain()->getMinimumLogSeverity(
                      ::mongo::logger::LogComponent::kAccessControl),
                  ::mongo::logger::LogSeverity::Log());

    // Query - not mentioned in configuration. should match default.
    ASSERT_FALSE(::mongo::logger::globalLogDomain()->hasMinimumLogSeverity(
        ::mongo::logger::LogComponent::kStorage));
    ASSERT_EQUALS(::mongo::logger::globalLogDomain()->getMinimumLogSeverity(
                      ::mongo::logger::LogComponent::kStorage),
                  ::mongo::logger::LogSeverity::Debug(verbosity));

    // Storage - cleared by -1 value in configuration. should match default.
    ASSERT_FALSE(::mongo::logger::globalLogDomain()->hasMinimumLogSeverity(
        ::mongo::logger::LogComponent::kStorage));
    ASSERT_EQUALS(::mongo::logger::globalLogDomain()->getMinimumLogSeverity(
                      ::mongo::logger::LogComponent::kStorage),
                  ::mongo::logger::LogSeverity::Debug(verbosity));

    // Journaling - explicitly set to 2 in configuration.
    ASSERT_TRUE(::mongo::logger::globalLogDomain()->hasMinimumLogSeverity(
        ::mongo::logger::LogComponent::kJournal));
    ASSERT_EQUALS(::mongo::logger::globalLogDomain()->getMinimumLogSeverity(
                      ::mongo::logger::LogComponent::kJournal),
                  ::mongo::logger::LogSeverity::Debug(2));
}

TEST(SetupOptions, Default) {
    std::vector<std::string> argv;
    argv.push_back("binaryname");

    ASSERT_OK(::mongo::setupServerOptions(argv));

    ASSERT_EQUALS(::mongo::serverGlobalParams.binaryName, "binaryname");
    ASSERT_FALSE(::mongo::serverGlobalParams.cwd.empty());
    ASSERT_EQUALS(::mongo::serverGlobalParams.argvArray.nFields(), static_cast<int>(argv.size()));
}

TEST(SetupOptions, RelativeBinaryPath) {
    std::vector<std::string> argv;
    argv.push_back("foo/bar/baz/binaryname");

    ASSERT_OK(::mongo::setupServerOptions(argv));

    ASSERT_EQUALS(::mongo::serverGlobalParams.binaryName, "binaryname");
    ASSERT_FALSE(::mongo::serverGlobalParams.cwd.empty());
    ASSERT_EQUALS(::mongo::serverGlobalParams.argvArray.nFields(), static_cast<int>(argv.size()));
}

TEST(SetupOptions, AbsoluteBinaryPath) {
    std::vector<std::string> argv;
    argv.push_back("/foo/bar/baz/binaryname");

    ASSERT_OK(::mongo::setupServerOptions(argv));

    ASSERT_EQUALS(::mongo::serverGlobalParams.binaryName, "binaryname");
    ASSERT_FALSE(::mongo::serverGlobalParams.cwd.empty());
    ASSERT_EQUALS(::mongo::serverGlobalParams.argvArray.nFields(), static_cast<int>(argv.size()));
}

TEST(SetupOptions, EmptyBinaryName) {
    std::vector<std::string> argv;
    argv.push_back("");

    ASSERT_OK(::mongo::setupServerOptions(argv));

    ASSERT_EQUALS(::mongo::serverGlobalParams.binaryName, "");
    ASSERT_FALSE(::mongo::serverGlobalParams.cwd.empty());
    ASSERT_EQUALS(::mongo::serverGlobalParams.argvArray.nFields(), static_cast<int>(argv.size()));
}

TEST(SetupOptions, MissingBinaryName) {
    std::vector<std::string> argv;

    ASSERT_NOT_OK(::mongo::setupServerOptions(argv));
}

#if !defined(_WIN32) && !(defined(__APPLE__) && TARGET_OS_TV)

#define ASSERT_BOOST_SUCCESS(ec) ASSERT_FALSE(ec) << ec.message()

TEST(SetupOptions, DeepCwd) {
    std::vector<std::string> argv;
    argv.push_back("binaryname");

    // Save the original cwd.
    boost::system::error_code ec;
    boost::filesystem::path cwd = boost::filesystem::current_path(ec);
    ASSERT_BOOST_SUCCESS(ec);

    // All work is done under a temporary base directory.
    ::mongo::StringBuilder sb;
    sb << "/tmp/deepcwd-" << getpid();
    boost::filesystem::path deepBaseDir = sb.str();

    auto cleanup = ::mongo::MakeGuard([&] {
        boost::filesystem::current_path(cwd, ec);
        boost::filesystem::remove_all(deepBaseDir, ec);
    });

    // Clear out any old base dir, and create an empty dir.
    boost::filesystem::remove_all(deepBaseDir, ec);
    ASSERT_BOOST_SUCCESS(ec);
    boost::filesystem::create_directories(deepBaseDir, ec);
    ASSERT_BOOST_SUCCESS(ec);

    // Resolve the canonical base dir, in case /tmp is a symlink (eg. on OSX).
    // This involves chdir into it, then getcwd() again (since getcwd() returns a canonical path).
    boost::filesystem::current_path(deepBaseDir, ec);
    ASSERT_BOOST_SUCCESS(ec);
    deepBaseDir = boost::filesystem::current_path(ec);
    ASSERT_BOOST_SUCCESS(ec);

    boost::filesystem::path deepCwd = deepBaseDir;
    boost::filesystem::path pathComponent = std::string(40, 'a');
    while (deepCwd.size() < 3800) {
        if (!boost::filesystem::create_directory(deepCwd / pathComponent, ec)) {
            // Could not mkdir, so we are done and will stick with the previous level, thanks.
            break;
        }

        boost::filesystem::current_path(deepCwd / pathComponent, ec);
        if (ec) {
            // Could not chdir, so we are done and will stick with the previous level, thanks.
            break;
        }

        deepCwd /= pathComponent;
    }

    // Run the actual test.
    Status res = ::mongo::setupServerOptions(argv);

    ASSERT_OK(res);
    ASSERT_FALSE(::mongo::serverGlobalParams.cwd.empty());
    ASSERT_EQUALS(::mongo::serverGlobalParams.cwd, deepCwd.native())
        << "serverGlobalParams.cwd is " << ::mongo::serverGlobalParams.cwd.size()
        << " bytes long and deepCwd is " << deepCwd.size() << " bytes long.";
    ASSERT_EQUALS(::mongo::serverGlobalParams.argvArray.nFields(), static_cast<int>(argv.size()));
}

TEST(SetupOptions, UnlinkedCwd) {
    std::vector<std::string> argv;
    argv.push_back("binaryname");

    // Save the original cwd.
    boost::system::error_code ec;
    boost::filesystem::path cwd = boost::filesystem::current_path(ec);
    ASSERT_BOOST_SUCCESS(ec);

    std::string unlinkDir;

    auto cleanup = ::mongo::MakeGuard([&] {
        boost::filesystem::current_path(cwd, ec);
        if (!unlinkDir.empty()) {
            boost::filesystem::remove(cwd / unlinkDir, ec);
        }
    });

    // mkdir our own unlink dir
    unsigned int i = 0;
    while (1) {
        ::mongo::StringBuilder sb;
        sb << "unlink" << i;
        unlinkDir = sb.str();
        if (boost::filesystem::create_directory(unlinkDir, ec)) {
            // New directory was created, great.
            break;
        } else {
            // Directory failed to create, or already existed (so try the next one).
            ASSERT_BOOST_SUCCESS(ec);
            i++;
        }
    }

    // chdir into the unlink dir
    boost::filesystem::current_path(unlinkDir, ec);
    ASSERT_BOOST_SUCCESS(ec);

    // Naive "rmdir ." doesn't work on Linux.
    // Naive rmdir of cwd doesn't work on Solaris doesn't work (no matter how it's specified).
    // So we use a subprocess to unlink the dir.
    pid_t pid = fork();
    ASSERT_NOT_EQUALS(pid, -1) << "unable to fork: " << ::mongo::errnoWithDescription();
    if (pid == 0) {
        // Subprocess
        // No exceptions, ASSERT(), FAIL() or logging.
        // Only signalling to the parent process via exit status.
        // Only quickexit via _exit() (which prevents the cleanup guard from running, which is
        // what we want).

        // cd to root
        boost::filesystem::current_path("/", ec);
        if (ec) {
            _exit(1);
        }

        // rmdir using absolute path
        boost::filesystem::remove(cwd / unlinkDir, ec);
        if (ec) {
            _exit(2);
        }

        _exit(0);
    }

    // Wait for the subprocess to finish running the above code and exit, and check its result.
    int status;
    ASSERT_NOT_EQUALS(-1, waitpid(pid, &status, 0));
    ASSERT(WIFEXITED(status));
    int exitstatus = WEXITSTATUS(status);
    if (exitstatus == 1) {
        FAIL("subprocess was unable to cd to /");
    } else if (exitstatus == 2) {
        FAIL("subprocess was unable to remove unlink dir");
    } else {
        ASSERT_EQUALS(0, exitstatus);
    }

    // Run the actual test.
    Status res = ::mongo::setupServerOptions(argv);

    ASSERT_NOT_OK(res);
    ASSERT_STRING_CONTAINS(res.toString(), "Cannot get current working directory");
    ASSERT_STRING_CONTAINS(res.toString(), "No such file or directory");  // message for ENOENT
}

#endif  // !defined(_WIN32)

}  // unnamed namespace
