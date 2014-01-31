/* Copyright 2013 10gen Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "mongo/bson/util/builder.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_options_helpers.h"
#include "mongo/logger/logger.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/options_parser/environment.h"
#include "mongo/util/options_parser/option_section.h"
#include "mongo/util/options_parser/options_parser.h"

namespace {

    using mongo::ErrorCodes;
    using mongo::Status;

    namespace moe = mongo::optionenvironment;

    class OptionsParserTester : public moe::OptionsParser {
    public:
        Status readConfigFile(const std::string& filename, std::string* config) {
            if (filename != _filename) {
                ::mongo::StringBuilder sb;
                sb << "Parser using filename: " << filename <<
                      " which does not match expected filename: " << _filename;
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

        ASSERT_OK(::mongo::storeServerOptions(environment, argv));

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

        ASSERT_OK(::mongo::storeServerOptions(environment, argv));

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

        ASSERT_OK(::mongo::storeServerOptions(environment, argv));

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

        ASSERT_OK(parser.run(options, argv, env_map, &environment));

        ASSERT_OK(::mongo::storeServerOptions(environment, argv));

        int verbosity = 0;
        ASSERT_EQUALS(::mongo::logger::globalLogDomain()->getMinimumLogSeverity(),
                      ::mongo::logger::LogSeverity::Debug(verbosity));
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

        ASSERT_NOT_OK(::mongo::storeServerOptions(environment, argv));
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

        ASSERT_OK(::mongo::storeServerOptions(environment, argv));

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

        ASSERT_NOT_OK(::mongo::storeServerOptions(environment, argv));
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

        ASSERT_OK(::mongo::storeServerOptions(environment, argv));

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

        ASSERT_OK(::mongo::storeServerOptions(environment, argv));

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

        ASSERT_OK(::mongo::storeServerOptions(environment, argv));

        int verbosity = 3;
        ASSERT_EQUALS(::mongo::logger::globalLogDomain()->getMinimumLogSeverity(),
                      ::mongo::logger::LogSeverity::Debug(verbosity));
    }

} // unnamed namespace
