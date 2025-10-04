/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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


#include <ostream>
#include <utility>

#include <boost/algorithm/string/join.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "boost/system/detail/error_code.hpp"
#include <fmt/format.h>

#ifndef _WIN32
#include <cstdlib>

#include <sys/wait.h>
#endif

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/base/parse_number.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/server_options.h"
#include "mongo/db/server_options_base.h"
#include "mongo/db/server_options_helpers.h"
#include "mongo/db/server_options_nongeneral_gen.h"
#include "mongo/db/server_options_server_helpers.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/tenant_id.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/unittest/log_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/options_parser/environment.h"
#include "mongo/util/options_parser/option_section.h"
#include "mongo/util/options_parser/options_parser.h"
#include "mongo/util/options_parser/value.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

#if defined(MONGO_CONFIG_HAVE_HEADER_UNISTD_H)
#include <unistd.h>
#endif

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

using mongo::ClusterRole;
using mongo::ErrorCodes;
using mongo::Status;

using mongo::unittest::getMinimumLogSeverity;
using mongo::unittest::hasMinimumLogSeverity;

namespace moe = mongo::optionenvironment;

MONGO_INITIALIZER(ServerLogRedirection)(mongo::InitializerContext*) {
    // ssl_options_server.cpp has an initializer which depends on logging.
    // We can stub that dependency out for unit testing purposes.
}

class OptionsParserTester : public moe::OptionsParser {
public:
    Status readConfigFile(const std::string& filename,
                          std::string* config,
                          moe::ConfigExpand) override {
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

class Verbosity : public mongo::unittest::Test {
    /** Reset the log level before we test */
    mongo::unittest::MinimumLoggedSeverityGuard _severityGuard{mongo::logv2::LogComponent::kDefault,
                                                               mongo::logv2::LogSeverity::Info()};
};

class SetupOptionsTestConfig {
public:
    SetupOptionsTestConfig() : binaryArgs_{}, configFileName_{}, configOpts_{}, envVars_{} {}

    SetupOptionsTestConfig(std::vector<std::string> binaryArgs,
                           std::string configFileName,
                           std::vector<std::pair<std::string, std::string>> configOpts,
                           std::vector<std::pair<std::string, std::string>> envVars)
        : binaryArgs_{binaryArgs},
          configFileName_{configFileName},
          configOpts_{configOpts},
          envVars_{envVars} {}

    std::vector<std::string> binaryArgs() const {
        std::vector<std::string> args = {"binaryname"};
        args.insert(args.end(), binaryArgs_.cbegin(), binaryArgs_.cend());

        if (!configFileName_.empty()) {
            args.push_back("--config");
            args.push_back(configFileName_);
        }

        return args;
    }

    std::vector<std::pair<std::string, std::string>> envVars() const {
        return envVars_;
    }

    bool hasEnvVars() const {
        return !envVars_.empty();
    }

    std::string configFileName() const {
        return configFileName_;
    }

    bool hasConfigFile() const {
        return !configFileName_.empty();
    }

    std::string configFileContents() const {
        std::string fileContents;
        for (auto&& [key, value] : configOpts_) {
            std::vector<std::string> splitKeys;
            str::splitStringDelim(key, &splitKeys, '.');

            // Split up the configuration key by '.' into separate sections.
            int indent = 0;
            for (auto&& splitKey : splitKeys) {
                if (indent > 0) {
                    fileContents += "\n";
                }
                fileContents += std::string(indent, ' ') + splitKey + ":";
                indent += 4;
            }
            fileContents += " " + value + "\n";
        }
        return fileContents;
    }

    std::string toString() const {
        std::string str = fmt::format("argv=[{}],", boost::algorithm::join(binaryArgs(), ", "));
        if (hasConfigFile()) {
            str += fmt::format("confFile=[\n{}],", configFileContents());
        }
        if (hasEnvVars()) {
            std::vector<std::string> envs;
            for (auto&& [k, v] : envVars_) {
                envs.push_back(fmt::format("{}={}", k, v));
            }
            str += fmt::format("env=[{}],", boost::algorithm::join(envs, ", "));
        }
        return fmt::format("SetupOptionsTestConfig({})", str);
    }

private:
    std::vector<std::string> binaryArgs_;
    std::string configFileName_;
    std::vector<std::pair<std::string, std::string>> configOpts_;
    std::vector<std::pair<std::string, std::string>> envVars_;
};

TEST_F(Verbosity, Default) {
    OptionsParserTester parser;
    moe::Environment environment;
    moe::OptionSection options;

    ASSERT_OK(::mongo::addGeneralServerOptions(&options));

    std::vector<std::string> argv;
    argv.push_back("binaryname");

    ASSERT_OK(parser.run(options, argv, &environment));

    ASSERT_OK(::mongo::validateServerOptions(environment));
    ASSERT_OK(::mongo::canonicalizeServerOptions(&environment));
    ASSERT_OK(::mongo::setupServerOptions(argv));
    ASSERT_OK(::mongo::storeServerOptions(environment));

    // Make sure the log level didn't change since we didn't specify any verbose options
    ASSERT_EQUALS(getMinimumLogSeverity(), ::mongo::logv2::LogSeverity::Info());
}

TEST_F(Verbosity, CommandLineImplicit) {
    OptionsParserTester parser;
    moe::Environment environment;
    moe::OptionSection options;

    ASSERT_OK(::mongo::addGeneralServerOptions(&options));

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--verbose");

    ASSERT_OK(parser.run(options, argv, &environment));

    ASSERT_OK(::mongo::validateServerOptions(environment));
    ASSERT_OK(::mongo::canonicalizeServerOptions(&environment));
    ASSERT_OK(::mongo::setupServerOptions(argv));
    ASSERT_OK(::mongo::storeServerOptions(environment));

    int verbosity = 1;
    ASSERT_EQUALS(getMinimumLogSeverity(), ::mongo::logv2::LogSeverity::Debug(verbosity));
}

TEST_F(Verbosity, CommandLineString) {
    OptionsParserTester parser;
    moe::Environment environment;
    moe::OptionSection options;

    ASSERT_OK(::mongo::addGeneralServerOptions(&options));

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--verbose");
    argv.push_back("vvvv");

    ASSERT_OK(parser.run(options, argv, &environment));

    ASSERT_OK(::mongo::validateServerOptions(environment));
    ASSERT_OK(::mongo::canonicalizeServerOptions(&environment));
    ASSERT_OK(::mongo::setupServerOptions(argv));
    ASSERT_OK(::mongo::storeServerOptions(environment));

    int verbosity = 4;
    ASSERT_EQUALS(getMinimumLogSeverity(), ::mongo::logv2::LogSeverity::Debug(verbosity));
}

TEST_F(Verbosity, CommandLineStringDisguisedLongForm) {
    OptionsParserTester parser;
    moe::Environment environment;
    moe::OptionSection options;

    ASSERT_OK(::mongo::addGeneralServerOptions(&options));

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("-verbose");
    argv.push_back("vvvv");

    ASSERT_OK(parser.run(options, argv, &environment));

    ASSERT_OK(::mongo::validateServerOptions(environment));
    ASSERT_OK(::mongo::canonicalizeServerOptions(&environment));
    ASSERT_OK(::mongo::setupServerOptions(argv));
    ASSERT_OK(::mongo::storeServerOptions(environment));

    int verbosity = 4;
    ASSERT_EQUALS(getMinimumLogSeverity(), ::mongo::logv2::LogSeverity::Debug(verbosity));
}

TEST_F(Verbosity, CommandLineEmptyString) {
    OptionsParserTester parser;
    moe::Environment environment;
    moe::OptionSection options;

    ASSERT_OK(::mongo::addGeneralServerOptions(&options));

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--verbose");
    argv.push_back("");

    ASSERT_NOT_OK(parser.run(options, argv, &environment));
}

TEST_F(Verbosity, CommandLineBadString) {
    OptionsParserTester parser;
    moe::Environment environment;
    moe::OptionSection options;

    ASSERT_OK(::mongo::addGeneralServerOptions(&options));

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--verbose");
    argv.push_back("beloud");

    ASSERT_OK(parser.run(options, argv, &environment));

    ASSERT_NOT_OK(::mongo::validateServerOptions(environment));
}

TEST_F(Verbosity, CommandLineBadStringOnlyDash) {
    OptionsParserTester parser;
    moe::Environment environment;
    moe::OptionSection options;

    ASSERT_OK(::mongo::addGeneralServerOptions(&options));

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("-");

    ASSERT_NOT_OK(parser.run(options, argv, &environment));
}

TEST_F(Verbosity, CommandLineBadStringOnlyTwoDashes) {
    OptionsParserTester parser;
    moe::Environment environment;
    moe::OptionSection options;

    ASSERT_OK(::mongo::addGeneralServerOptions(&options));

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--");

    ASSERT_OK(parser.run(options, argv, &environment));
}

TEST_F(Verbosity, INIConfigString) {
    OptionsParserTester parser;
    moe::Environment environment;
    moe::OptionSection options;

    ASSERT_OK(::mongo::addGeneralServerOptions(&options));

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.ini");

    parser.setConfig("config.ini", "verbose=vvvv");

    ASSERT_OK(parser.run(options, argv, &environment));

    ASSERT_OK(::mongo::validateServerOptions(environment));
    ASSERT_OK(::mongo::canonicalizeServerOptions(&environment));
    ASSERT_OK(::mongo::setupServerOptions(argv));
    ASSERT_OK(::mongo::storeServerOptions(environment));

    int verbosity = 4;
    ASSERT_EQUALS(getMinimumLogSeverity(), ::mongo::logv2::LogSeverity::Debug(verbosity));
}

TEST_F(Verbosity, INIConfigBadString) {
    OptionsParserTester parser;
    moe::Environment environment;
    moe::OptionSection options;

    ASSERT_OK(::mongo::addGeneralServerOptions(&options));

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.ini");

    parser.setConfig("config.ini", "verbose=beloud");

    ASSERT_OK(parser.run(options, argv, &environment));

    ASSERT_NOT_OK(::mongo::validateServerOptions(environment));
}

TEST_F(Verbosity, INIConfigEmptyString) {
    OptionsParserTester parser;
    moe::Environment environment;
    moe::OptionSection options;

    ASSERT_OK(::mongo::addGeneralServerOptions(&options));

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.ini");

    parser.setConfig("config.ini", "verbose=");

    ASSERT_OK(parser.run(options, argv, &environment));

    ASSERT_OK(::mongo::validateServerOptions(environment));
    ASSERT_OK(::mongo::canonicalizeServerOptions(&environment));
    ASSERT_OK(::mongo::setupServerOptions(argv));
    ASSERT_OK(::mongo::storeServerOptions(environment));

    int verbosity = 0;
    ASSERT_EQUALS(getMinimumLogSeverity(), ::mongo::logv2::LogSeverity::Debug(verbosity));
}

TEST_F(Verbosity, JSONConfigString) {
    OptionsParserTester parser;
    moe::Environment environment;
    moe::OptionSection options;

    ASSERT_OK(::mongo::addGeneralServerOptions(&options));

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.json");

    parser.setConfig("config.json", "{ \"systemLog.verbosity\" : 4 }");

    ASSERT_OK(parser.run(options, argv, &environment));

    ASSERT_OK(::mongo::validateServerOptions(environment));
    ASSERT_OK(::mongo::canonicalizeServerOptions(&environment));
    ASSERT_OK(::mongo::setupServerOptions(argv));
    ASSERT_OK(::mongo::storeServerOptions(environment));

    int verbosity = 4;
    ASSERT_EQUALS(getMinimumLogSeverity(), ::mongo::logv2::LogSeverity::Debug(verbosity));
}

TEST_F(Verbosity, MultipleSourcesMultipleOptions) {
    OptionsParserTester parser;
    moe::Environment environment;
    moe::OptionSection options;

    ASSERT_OK(::mongo::addGeneralServerOptions(&options));

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.json");
    argv.push_back("--verbose");
    argv.push_back("vvv");

    parser.setConfig("config.json", "{ \"systemLog.verbosity\" : 4 }");

    ASSERT_OK(parser.run(options, argv, &environment));

    ASSERT_OK(::mongo::validateServerOptions(environment));
    ASSERT_OK(::mongo::canonicalizeServerOptions(&environment));
    ASSERT_OK(::mongo::setupServerOptions(argv));
    ASSERT_OK(::mongo::storeServerOptions(environment));

    int verbosity = 3;
    ASSERT_EQUALS(getMinimumLogSeverity(), ::mongo::logv2::LogSeverity::Debug(verbosity));
}

TEST_F(Verbosity, YAMLConfigStringLogComponent) {
    OptionsParserTester parser;
    moe::Environment environment;
    moe::OptionSection options;

    // Log level for Storage will be cleared by config file value.
    auto storageSeverityGuard = mongo::unittest::MinimumLoggedSeverityGuard{
        mongo::logv2::LogComponent::kStorage, mongo::logv2::LogSeverity::Debug(1)};

    ASSERT_OK(::mongo::addGeneralServerOptions(&options));

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.yaml");

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

    ASSERT_OK(parser.run(options, argv, &environment));

    ASSERT_OK(::mongo::validateServerOptions(environment));
    ASSERT_OK(::mongo::canonicalizeServerOptions(&environment));
    ASSERT_OK(::mongo::setupServerOptions(argv));
    ASSERT_OK(::mongo::storeServerOptions(environment));

    // Verify component log levels using global log domain.
    int verbosity = 4;

    // Default
    ASSERT_EQUALS(getMinimumLogSeverity(), ::mongo::logv2::LogSeverity::Debug(verbosity));
    ASSERT_EQUALS(getMinimumLogSeverity(::mongo::logv2::LogComponent::kDefault),
                  ::mongo::logv2::LogSeverity::Debug(verbosity));

    // AccessControl
    ASSERT_TRUE(hasMinimumLogSeverity(::mongo::logv2::LogComponent::kAccessControl));
    ASSERT_EQUALS(getMinimumLogSeverity(::mongo::logv2::LogComponent::kAccessControl),
                  ::mongo::logv2::LogSeverity::Log());

    // Query - not mentioned in configuration. should match default.
    ASSERT_FALSE(hasMinimumLogSeverity(::mongo::logv2::LogComponent::kStorage));
    ASSERT_EQUALS(getMinimumLogSeverity(::mongo::logv2::LogComponent::kStorage),
                  ::mongo::logv2::LogSeverity::Debug(verbosity));

    // Storage - cleared by -1 value in configuration. should match default.
    ASSERT_FALSE(hasMinimumLogSeverity(::mongo::logv2::LogComponent::kStorage));
    ASSERT_EQUALS(getMinimumLogSeverity(::mongo::logv2::LogComponent::kStorage),
                  ::mongo::logv2::LogSeverity::Debug(verbosity));

    // Journaling - explicitly set to 2 in configuration.
    ASSERT_TRUE(hasMinimumLogSeverity(::mongo::logv2::LogComponent::kJournal));
    ASSERT_EQUALS(getMinimumLogSeverity(::mongo::logv2::LogComponent::kJournal),
                  ::mongo::logv2::LogSeverity::Debug(2));
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

TEST(SetupOptions, SlowMsCommandLineParamParsesSuccessfully) {
    OptionsParserTester parser;
    moe::Environment environment;
    moe::OptionSection options;

    ASSERT_OK(::mongo::addNonGeneralServerOptions(&options));

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--slowms");
    argv.push_back("300");

    ASSERT_OK(parser.run(options, argv, &environment));

    ASSERT_OK(::mongo::validateServerOptions(environment));
    ASSERT_OK(::mongo::canonicalizeServerOptions(&environment));
    ASSERT_OK(::mongo::setupServerOptions(argv));
    ASSERT_OK(::mongo::storeServerOptions(environment));

    ASSERT_EQ(::mongo::serverGlobalParams.slowMS.load(), 300);
}

TEST(SetupOptions, SlowMsParamInitializedSuccessfullyFromINIConfigFile) {
    OptionsParserTester parser;
    moe::Environment environment;
    moe::OptionSection options;

    ASSERT_OK(::mongo::addGeneralServerOptions(&options));
    ASSERT_OK(::mongo::addNonGeneralServerOptions(&options));

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.ini");

    parser.setConfig("config.ini", "slowms=300");

    ASSERT_OK(parser.run(options, argv, &environment));

    ASSERT_OK(::mongo::validateServerOptions(environment));
    ASSERT_OK(::mongo::canonicalizeServerOptions(&environment));
    ASSERT_OK(::mongo::setupServerOptions(argv));
    ASSERT_OK(::mongo::storeServerOptions(environment));

    ASSERT_EQ(::mongo::serverGlobalParams.slowMS.load(), 300);
}

TEST(SetupOptions, SlowMsParamInitializedSuccessfullyFromYAMLConfigFile) {
    OptionsParserTester parser;
    moe::Environment environment;
    moe::OptionSection options;

    ASSERT_OK(::mongo::addGeneralServerOptions(&options));
    ASSERT_OK(::mongo::addNonGeneralServerOptions(&options));

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.yaml");

    parser.setConfig("config.yaml",
                     "operationProfiling:\n"
                     "    slowOpThresholdMs: 300\n");

    ASSERT_OK(parser.run(options, argv, &environment));

    ASSERT_OK(::mongo::validateServerOptions(environment));
    ASSERT_OK(::mongo::canonicalizeServerOptions(&environment));
    ASSERT_OK(::mongo::setupServerOptions(argv));
    ASSERT_OK(::mongo::storeServerOptions(environment));

    ASSERT_EQ(::mongo::serverGlobalParams.slowMS.load(), 300);
}

TEST(SetupOptions, NonNumericSlowMsCommandLineOptionFailsToParse) {
    OptionsParserTester parser;
    moe::Environment environment;
    moe::OptionSection options;

    ASSERT_OK(::mongo::addNonGeneralServerOptions(&options));

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--slowms");
    argv.push_back("invalid");

    ASSERT_NOT_OK(parser.run(options, argv, &environment));
}

TEST(SetupOptions, NonNumericSlowMsYAMLConfigOptionFailsToParse) {
    OptionsParserTester parser;
    moe::Environment environment;
    moe::OptionSection options;

    ASSERT_OK(::mongo::addNonGeneralServerOptions(&options));

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.yaml");

    parser.setConfig("config.yaml",
                     "operationProfiling:\n"
                     "    slowOpThresholdMs: invalid\n");

    ASSERT_NOT_OK(parser.run(options, argv, &environment));
}

TEST(SetupOptions, SampleRateCommandLineParamParsesSuccessfully) {
    OptionsParserTester parser;
    moe::Environment environment;
    moe::OptionSection options;

    ASSERT_OK(::mongo::addNonGeneralServerOptions(&options));

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--slowOpSampleRate");
    argv.push_back("0.5");

    ASSERT_OK(parser.run(options, argv, &environment));

    ASSERT_OK(::mongo::validateServerOptions(environment));
    ASSERT_OK(::mongo::canonicalizeServerOptions(&environment));
    ASSERT_OK(::mongo::setupServerOptions(argv));
    ASSERT_OK(::mongo::storeServerOptions(environment));

    ASSERT_EQ(::mongo::serverGlobalParams.sampleRate.load(), 0.5);
}

TEST(SetupOptions, SampleRateParamInitializedSuccessfullyFromINIConfigFile) {
    OptionsParserTester parser;
    moe::Environment environment;
    moe::OptionSection options;

    ASSERT_OK(::mongo::addGeneralServerOptions(&options));
    ASSERT_OK(::mongo::addNonGeneralServerOptions(&options));

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.ini");

    parser.setConfig("config.ini", "slowOpSampleRate=0.5");

    ASSERT_OK(parser.run(options, argv, &environment));

    ASSERT_OK(::mongo::validateServerOptions(environment));
    ASSERT_OK(::mongo::canonicalizeServerOptions(&environment));
    ASSERT_OK(::mongo::setupServerOptions(argv));
    ASSERT_OK(::mongo::storeServerOptions(environment));

    ASSERT_EQ(::mongo::serverGlobalParams.sampleRate.load(), 0.5);
}

TEST(SetupOptions, SampleRateParamInitializedSuccessfullyFromYAMLConfigFile) {
    OptionsParserTester parser;
    moe::Environment environment;
    moe::OptionSection options;

    ASSERT_OK(::mongo::addGeneralServerOptions(&options));
    ASSERT_OK(::mongo::addNonGeneralServerOptions(&options));

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.yaml");

    parser.setConfig("config.yaml",
                     "operationProfiling:\n"
                     "    slowOpSampleRate: 0.5\n");

    ASSERT_OK(parser.run(options, argv, &environment));

    ASSERT_OK(::mongo::validateServerOptions(environment));
    ASSERT_OK(::mongo::canonicalizeServerOptions(&environment));
    ASSERT_OK(::mongo::setupServerOptions(argv));
    ASSERT_OK(::mongo::storeServerOptions(environment));

    ASSERT_EQ(::mongo::serverGlobalParams.sampleRate.load(), 0.5);
}

TEST(SetupOptions, NonNumericSampleRateCommandLineOptionFailsToParse) {
    OptionsParserTester parser;
    moe::Environment environment;
    moe::OptionSection options;

    ASSERT_OK(::mongo::addNonGeneralServerOptions(&options));

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--slowOpSampleRate");
    argv.push_back("invalid");

    ASSERT_NOT_OK(parser.run(options, argv, &environment));
}

TEST(SetupOptions, NonNumericSampleRateYAMLConfigOptionFailsToParse) {
    OptionsParserTester parser;
    moe::Environment environment;
    moe::OptionSection options;


    ASSERT_OK(::mongo::addNonGeneralServerOptions(&options));

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.yaml");

    parser.setConfig("config.yaml",
                     "operationProfiling:\n"
                     "    slowOpSampleRate: invalid\n");

    ASSERT_NOT_OK(parser.run(options, argv, &environment));
}

TEST(SetupOptions, SlowTaskExecutorWaitTimeProfilingMsCommandLineParamParsesSuccessfully) {
    OptionsParserTester parser;
    moe::Environment environment;
    moe::OptionSection options;

    ASSERT_OK(addNonGeneralServerOptions(&options));

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--slowTaskWaitTimeProfilingMs");
    argv.push_back("200");

    ASSERT_OK(parser.run(options, argv, &environment));

    ASSERT_OK(validateServerOptions(environment));
    ASSERT_OK(canonicalizeServerOptions(&environment));
    ASSERT_OK(setupServerOptions(argv));
    ASSERT_OK(storeServerOptions(environment));

    ASSERT_EQ(serverGlobalParams.slowTaskExecutorWaitTimeProfilingMs.load(), 200);
}

TEST(SetupOptions,
     SlowTaskExecutorWaitTimeProfilingMsParamInitializedSuccessfullyFromINIConfigFile) {
    OptionsParserTester parser;
    moe::Environment environment;
    moe::OptionSection options;

    ASSERT_OK(addGeneralServerOptions(&options));
    ASSERT_OK(addNonGeneralServerOptions(&options));

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.ini");

    parser.setConfig("config.ini", "slowTaskWaitTimeProfilingMs=200");

    ASSERT_OK(parser.run(options, argv, &environment));

    ASSERT_OK(validateServerOptions(environment));
    ASSERT_OK(canonicalizeServerOptions(&environment));
    ASSERT_OK(setupServerOptions(argv));
    ASSERT_OK(storeServerOptions(environment));

    ASSERT_EQ(serverGlobalParams.slowTaskExecutorWaitTimeProfilingMs.load(), 200);
}

TEST(SetupOptions,
     SlowTaskExecutorWaitTimeProfilingMsParamInitializedSuccessfullyFromYAMLConfigFile) {
    OptionsParserTester parser;
    moe::Environment environment;
    moe::OptionSection options;

    ASSERT_OK(addGeneralServerOptions(&options));
    ASSERT_OK(addNonGeneralServerOptions(&options));

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.yaml");

    parser.setConfig("config.yaml",
                     "taskExecutorProfiling:\n"
                     "    slowTaskWaitTimeProfilingMs: 200\n");

    ASSERT_OK(parser.run(options, argv, &environment));

    ASSERT_OK(validateServerOptions(environment));
    ASSERT_OK(canonicalizeServerOptions(&environment));
    ASSERT_OK(setupServerOptions(argv));
    ASSERT_OK(storeServerOptions(environment));

    ASSERT_EQ(serverGlobalParams.slowTaskExecutorWaitTimeProfilingMs.load(), 200);
}

TEST(SetupOptions, NonNumericSlowTaskExecutorWaitTimeProfilingMsCommandLineOptionFailsToParse) {
    OptionsParserTester parser;
    moe::Environment environment;
    moe::OptionSection options;

    ASSERT_OK(addNonGeneralServerOptions(&options));

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--slowTaskWaitTimeProfilingMs");
    argv.push_back("invalid");

    ASSERT_NOT_OK(parser.run(options, argv, &environment));
}

TEST(SetupOptions, NonNumericlowTaskExecutorWaitTimeProfilingMsYAMLConfigOptionFailsToParse) {
    OptionsParserTester parser;
    moe::Environment environment;
    moe::OptionSection options;

    ASSERT_OK(addNonGeneralServerOptions(&options));

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.yaml");

    parser.setConfig("config.yaml",
                     "taskExecutorProfiling:\n"
                     "    slowTaskWaitTimeProfilingMs: invalid\n");

    ASSERT_NOT_OK(parser.run(options, argv, &environment));
}

#if !defined(_WIN32) && !defined(__APPLE__)
class ForkTestSpec {
public:
    enum Value {
        NO_OPTIONS,
        COMMANDLINE_ONLY,
        CONFIG_ONLY,
        BOTH,
        COMMANDLINE_WITH_ENV,
        CONFIG_WITH_ENV,
        BOTH_WITH_ENV,
    };

    ForkTestSpec() = default;
    constexpr operator Value() const {
        return value_;
    }
    explicit operator bool() = delete;

    ForkTestSpec(Value value) : value_{value} {
        std::vector<std::string> args = {"--fork", "--syslog"};
        std::vector<std::pair<std::string, std::string>> envVars{
            {"MONGODB_CONFIG_OVERRIDE_NOFORK", "1"}};
        std::string confFileName = "config.yaml";
        std::vector<std::pair<std::string, std::string>> opts = {
            {"systemLog.destination", "syslog"}, {"processManagement.fork", "true"}};

        switch (value) {
            case NO_OPTIONS: {
                config_ = SetupOptionsTestConfig();
                name_ = "NO_OPTIONS";
                break;
            }
            case COMMANDLINE_ONLY: {
                config_ = SetupOptionsTestConfig(args, "", {}, {});
                name_ = "COMMANDLINE_ONLY";
                break;
            }
            case CONFIG_ONLY: {
                config_ = SetupOptionsTestConfig({}, confFileName, opts, {});
                name_ = "CONFIG_ONLY";
                break;
            }
            case BOTH: {
                config_ = SetupOptionsTestConfig(args, confFileName, opts, {});
                name_ = "BOTH";
                break;
            }
            case COMMANDLINE_WITH_ENV: {
                config_ = SetupOptionsTestConfig(args, "", {}, envVars);
                name_ = "COMMANDLINE_WITH_ENV";
                break;
            }
            case CONFIG_WITH_ENV: {
                config_ = SetupOptionsTestConfig({}, confFileName, opts, envVars);
                name_ = "CONFIG_WITH_ENV";
                break;
            }
            case BOTH_WITH_ENV: {
                config_ = SetupOptionsTestConfig(args, confFileName, opts, envVars);
                name_ = "BOTH_WITH_ENV";
                break;
            }
        }
    }

    SetupOptionsTestConfig config() const {
        return config_;
    }

    std::string toString() const {
        return fmt::format(
            "SetupOptionsTestConfig(specName={}, config={})", name_, config_.toString());
    }

private:
    Value value_;
    std::string name_;
    SetupOptionsTestConfig config_;
};

class ForkTest {
public:
    ForkTest(const ForkTestSpec spec, bool doForkValue)
        : spec_{spec}, doForkValue_{doForkValue}, parser_{}, environment_{}, options_{} {
        ASSERT_OK(addGeneralServerOptions(&options_));
        ASSERT_OK(addNonGeneralServerOptions(&options_));

        if (spec.config().hasConfigFile()) {
            parser_.setConfig(spec.config().configFileName(), spec.config().configFileContents());
        }
    }

    void run() {
        auto&& argv = spec_.config().binaryArgs();

        for (auto&& [envKey, envValue] : spec_.config().envVars()) {
            setenv(envKey.c_str(), envValue.c_str(), 1);
        }

        ScopeGuard sg = [&] {
            serverGlobalParams.doFork = false;
            for (auto&& [envKey, envValue] : spec_.config().envVars()) {
                unsetenv(envKey.c_str());
            }
        };

        ASSERT_OK(parser_.run(options_, argv, &environment_)) << spec_.toString();

        ASSERT_OK(validateServerOptions(environment_)) << spec_.toString();
        ASSERT_OK(canonicalizeServerOptions(&environment_)) << spec_.toString();
        ASSERT_OK(setupServerOptions(argv)) << spec_.toString();
        ASSERT_OK(storeServerOptions(environment_)) << spec_.toString();

        ASSERT_EQ(serverGlobalParams.doFork, doForkValue_) << spec_.toString();
    }

private:
    ForkTestSpec spec_;
    bool doForkValue_;
    OptionsParserTester parser_;
    moe::Environment environment_;
    moe::OptionSection options_;
};

TEST(SetupOptions, ForkCommandLineParamParsesSuccessfully) {
    ForkTest{ForkTestSpec::NO_OPTIONS, false}.run();
    ForkTest{ForkTestSpec::COMMANDLINE_ONLY, true}.run();
}

TEST(SetupOptions, ForkYAMLConfigOptionParsesSuccessfully) {
    ForkTest{ForkTestSpec::NO_OPTIONS, false}.run();
    ForkTest{ForkTestSpec::CONFIG_ONLY, true}.run();
}

TEST(SetupOptions, ForkOptionAlwaysFalseWithNoforkEnvVar) {
    ForkTest{ForkTestSpec::COMMANDLINE_WITH_ENV, false}.run();
    ForkTest{ForkTestSpec::CONFIG_WITH_ENV, false}.run();
    ForkTest{ForkTestSpec::BOTH_WITH_ENV, false}.run();
}
#endif

TEST(ClusterRole, MonoRole) {
    const ClusterRole noRole{ClusterRole::None};
    ASSERT_TRUE(noRole.has(ClusterRole::None));
    ASSERT_FALSE(noRole.has(ClusterRole::ShardServer));
    ASSERT_FALSE(noRole.has(ClusterRole::ConfigServer));
    ASSERT_FALSE(noRole.has(ClusterRole::RouterServer));
    ASSERT_TRUE(noRole.hasExclusively(ClusterRole::None));
    ASSERT_FALSE(noRole.hasExclusively(ClusterRole::ShardServer));
    ASSERT_FALSE(noRole.hasExclusively(ClusterRole::ConfigServer));
    ASSERT_FALSE(noRole.hasExclusively(ClusterRole::RouterServer));

    const ClusterRole shardRole{ClusterRole::ShardServer};
    ASSERT_FALSE(shardRole.has(ClusterRole::None));
    ASSERT_TRUE(shardRole.has(ClusterRole::ShardServer));
    ASSERT_FALSE(shardRole.has(ClusterRole::ConfigServer));
    ASSERT_FALSE(shardRole.has(ClusterRole::RouterServer));
    ASSERT_FALSE(shardRole.hasExclusively(ClusterRole::None));
    ASSERT_TRUE(shardRole.hasExclusively(ClusterRole::ShardServer));
    ASSERT_FALSE(shardRole.hasExclusively(ClusterRole::ConfigServer));
    ASSERT_FALSE(shardRole.hasExclusively(ClusterRole::RouterServer));

    // Role cannot be set to config server only.

    const ClusterRole routerRole{ClusterRole::RouterServer};
    ASSERT_FALSE(routerRole.has(ClusterRole::None));
    ASSERT_FALSE(routerRole.has(ClusterRole::ShardServer));
    ASSERT_FALSE(routerRole.has(ClusterRole::ConfigServer));
    ASSERT_TRUE(routerRole.has(ClusterRole::RouterServer));
    ASSERT_FALSE(routerRole.hasExclusively(ClusterRole::None));
    ASSERT_FALSE(routerRole.hasExclusively(ClusterRole::ShardServer));
    ASSERT_FALSE(routerRole.hasExclusively(ClusterRole::ConfigServer));
    ASSERT_TRUE(routerRole.hasExclusively(ClusterRole::RouterServer));
}

TEST(ClusterRole, MultiRole) {
    const ClusterRole shardAndConfigRole{ClusterRole::ShardServer, ClusterRole::ConfigServer};
    ASSERT_FALSE(shardAndConfigRole.has(ClusterRole::None));
    ASSERT_TRUE(shardAndConfigRole.has(ClusterRole::ShardServer));
    ASSERT_TRUE(shardAndConfigRole.has(ClusterRole::ConfigServer));
    ASSERT_FALSE(shardAndConfigRole.has(ClusterRole::RouterServer));
    ASSERT_FALSE(shardAndConfigRole.hasExclusively(ClusterRole::None));
    ASSERT_FALSE(shardAndConfigRole.hasExclusively(ClusterRole::ShardServer));
    ASSERT_FALSE(shardAndConfigRole.hasExclusively(ClusterRole::ConfigServer));
    ASSERT_FALSE(shardAndConfigRole.hasExclusively(ClusterRole::RouterServer));
}

TEST(ClusterRole, ToBson) {
    using R = ClusterRole;
    const auto s = R::ShardServer;
    const auto c = R::ConfigServer;
    const auto r = R::RouterServer;

    struct Case {
        R actual;
        std::vector<std::string> expected;
    };

    const std::vector<Case> allCases{
        {R{}, {}},
        {R{s}, {"shard"}},
        // {R{c}, {"config"}}, // Role cannot be set to config server only.
        {R{s, c}, {"shard", "config"}},
        {R{r}, {"router"}},
        {R{s, r}, {"shard", "router"}},
        {R{c, r}, {"config", "router"}},
        {R{s, c, r}, {"shard", "config", "router"}},
    };

    for (auto&& [actual, expected] : allCases) {
        BSONArrayBuilder bab;
        for (auto&& roleStr : expected) {
            bab.append(roleStr);
        }
        ASSERT_BSONOBJ_EQ(bab.arr(), toBSON(actual));
    }
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

    ::mongo::ScopeGuard cleanup = [&] {
        boost::filesystem::current_path(cwd, ec);
        boost::filesystem::remove_all(deepBaseDir, ec);
    };

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

    ::mongo::ScopeGuard cleanup = [&] {
        boost::filesystem::current_path(cwd, ec);
        if (!unlinkDir.empty()) {
            boost::filesystem::remove(cwd / unlinkDir, ec);
        }
    };

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
    if (pid == -1) {
        auto ec = lastSystemError();
        FAIL("unable to fork") << errorMessage(ec);
    }
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

class SetParameterOptionTest : public unittest::Test {
public:
    class TestServerParameter : public ServerParameter {
    public:
        TestServerParameter(StringData name, ServerParameterType spt, int x)
            : ServerParameter(name, spt), val{x} {}

        void append(OperationContext*,
                    BSONObjBuilder* bob,
                    StringData name,
                    const boost::optional<TenantId>&) final {
            bob->append(name, val);
        }

        Status setFromString(StringData str, const boost::optional<TenantId>&) final {
            int value;
            Status status = NumberParser{}(str, &value);
            if (!status.isOK())
                return status;
            val = value;
            return Status::OK();
        }

        int val;
    };
};

TEST_F(SetParameterOptionTest, ApplySetParameters) {
    auto param1 =
        std::make_unique<TestServerParameter>("p1", ServerParameterType::kStartupOnly, 123);
    auto param2 =
        std::make_unique<TestServerParameter>("p2", ServerParameterType::kStartupOnly, 234);
    auto p1 = param1.get();
    auto p2 = param2.get();
    ServerParameterSet paramSet;
    paramSet.add(std::move(param1));
    paramSet.add(std::move(param2));

    auto swObj =
        server_options_detail::applySetParameterOptions({{"p1", "555"}, {"p2", "666"}}, paramSet);
    ASSERT_OK(swObj);
    ASSERT_BSONOBJ_EQ(swObj.getValue(),
                      BSON("p1" << BSON("default" << 123 << "value" << 555) << "p2"
                                << BSON("default" << 234 << "value" << 666)));
    ASSERT_EQ(p1->val, 555);
    ASSERT_EQ(p2->val, 666);
}

}  // namespace
}  // namespace mongo
