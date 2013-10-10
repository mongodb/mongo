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

#include <map>
#include <sstream>

#include "mongo/bson/util/builder.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/options_parser/environment.h"
#include "mongo/util/options_parser/option_description.h"
#include "mongo/util/options_parser/option_section.h"
#include "mongo/util/options_parser/options_parser.h"

namespace {

    using mongo::ErrorCodes;
    using mongo::Status;

    namespace moe = mongo::optionenvironment;

#define TEST_CONFIG_PATH(x) "src/mongo/util/options_parser/test_config_files/" x

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

    TEST(Registration, DuplicateSingleName) {
        moe::OptionSection testOpts;
        try {
            testOpts.addOptionChaining("dup", "dup", moe::Switch, "dup");
            testOpts.addOptionChaining("new", "dup", moe::Switch, "dup");
            FAIL("Was able to register duplicate single name");
        }
        catch (::mongo::DBException &e) {
        }
    }

    TEST(Registration, DuplicateDottedName) {
        moe::OptionSection testOpts;
        try {
            testOpts.addOptionChaining("dup", "dup", moe::Switch, "dup");
            testOpts.addOptionChaining("dup", "new", moe::Switch, "dup");
            FAIL("Was able to register duplicate single name");
        }
        catch (::mongo::DBException &e) {
        }
    }

    TEST(Registration, DuplicatePositional) {
        moe::OptionSection testOpts;
        ASSERT_OK(testOpts.addPositionalOption(moe::PositionalOptionDescription("positional",
                                                                                moe::Int)));
        ASSERT_NOT_OK(testOpts.addPositionalOption(moe::PositionalOptionDescription("positional",
                                                                                    moe::Int)));
    }

    TEST(Registration, DefaultValueWrongType) {
        moe::OptionSection testOpts;
        try {
            testOpts.addOptionChaining("port", "port", moe::Int, "Port")
                                      .setDefault(moe::Value("String"));
            FAIL("Was able to register default value with wrong type");
        }
        catch (::mongo::DBException &e) {
        }
    }

    TEST(Registration, ImplicitValueWrongType) {
        moe::OptionSection testOpts;
        try {
            testOpts.addOptionChaining("port", "port", moe::Int, "Port")
                                      .setImplicit(moe::Value("String"));
            FAIL("Was able to register implicit value with wrong type");
        }
        catch (::mongo::DBException &e) {
        }
    }

    TEST(Registration, ComposableNotVector) {
        moe::OptionSection testOpts;
        try {
            testOpts.addOptionChaining("setParameter", "setParameter", moe::String,
                                       "Multiple Values").composing();
            FAIL("Was able to register composable option with wrong type");
        }
        catch (::mongo::DBException &e) {
        }
    }

    TEST(Registration, ComposableWithImplicit) {
        moe::OptionSection testOpts;
        try {
            std::vector<std::string> implicitVal;
            implicitVal.push_back("implicit");
            testOpts.addOptionChaining("setParameter", "setParameter", moe::StringVector,
                                       "Multiple Values")
                                      .setImplicit(moe::Value(implicitVal))
                                      .composing();
            FAIL("Was able to register composable option with implicit value");
        }
        catch (::mongo::DBException &e) {
        }

        try {
            std::vector<std::string> implicitVal;
            implicitVal.push_back("implicit");
            testOpts.addOptionChaining("setParameter", "setParameter", moe::StringVector,
                                       "Multiple Values")
                                      .composing()
                                      .setImplicit(moe::Value(implicitVal));
            FAIL("Was able to set implicit value on composable option");
        }
        catch (::mongo::DBException &e) {
        }
    }

    TEST(Registration, ComposableWithDefault) {
        moe::OptionSection testOpts;
        try {
            std::vector<std::string> defaultVal;
            defaultVal.push_back("default");
            testOpts.addOptionChaining("setParameter", "setParameter", moe::StringVector,
                                       "Multiple Values")
                                      .setDefault(moe::Value(defaultVal))
                                      .composing();
            FAIL("Was able to register composable option with default value");
        }
        catch (::mongo::DBException &e) {
        }

        try {
            std::vector<std::string> defaultVal;
            defaultVal.push_back("default");
            testOpts.addOptionChaining("setParameter", "setParameter", moe::StringVector,
                                       "Multiple Values")
                                      .composing()
                                      .setDefault(moe::Value(defaultVal));
            FAIL("Was able to set default value on composable option");
        }
        catch (::mongo::DBException &e) {
        }
    }

    TEST(Parsing, Good) {
        moe::OptionsParser parser;
        moe::Environment environment;

        moe::OptionSection testOpts;
        testOpts.addOptionChaining("help", "help", moe::Switch, "Display help");
        testOpts.addOptionChaining("port", "port", moe::Int, "Port");

        std::vector<std::string> argv;
        argv.push_back("binaryname");
        argv.push_back("--port");
        argv.push_back("5");
        argv.push_back("--help");
        std::map<std::string, std::string> env_map;

        moe::Value value;
        ASSERT_OK(parser.run(testOpts, argv, env_map, &environment));
        ASSERT_OK(environment.get(moe::Key("help"), &value));
        ASSERT_OK(environment.get(moe::Key("port"), &value));
        int port;
        ASSERT_OK(value.get(&port));
        ASSERT_EQUALS(port, 5);
    }

    TEST(Parsing, SubSection) {
        moe::OptionsParser parser;
        moe::Environment environment;

        moe::OptionSection testOpts;
        moe::OptionSection subSection("Section Name");

        subSection.addOptionChaining("port", "port", moe::Int, "Port");
        testOpts.addSection(subSection);

        std::vector<std::string> argv;
        argv.push_back("binaryname");
        argv.push_back("--port");
        argv.push_back("5");
        std::map<std::string, std::string> env_map;

        moe::Value value;
        ASSERT_OK(parser.run(testOpts, argv, env_map, &environment));
        ASSERT_OK(environment.get(moe::Key("port"), &value));
        int port;
        ASSERT_OK(value.get(&port));
        ASSERT_EQUALS(port, 5);
    }

    TEST(Parsing, StringVector) {
        moe::OptionsParser parser;
        moe::Environment environment;

        moe::OptionSection testOpts;
        testOpts.addOptionChaining("multival", "multival", moe::StringVector, "Multiple Values");

        std::vector<std::string> argv;
        argv.push_back("binaryname");
        argv.push_back("--multival");
        argv.push_back("val1");
        argv.push_back("--multival");
        argv.push_back("val2");
        std::map<std::string, std::string> env_map;

        moe::Value value;
        ASSERT_OK(parser.run(testOpts, argv, env_map, &environment));
        ASSERT_OK(environment.get(moe::Key("multival"), &value));
        std::vector<std::string> multival;
        std::vector<std::string>::iterator multivalit;
        ASSERT_OK(value.get(&multival));
        multivalit = multival.begin();
        ASSERT_EQUALS(*multivalit, "val1");
        multivalit++;
        ASSERT_EQUALS(*multivalit, "val2");
    }

    TEST(Parsing, Positional) {
        moe::OptionsParser parser;
        moe::Environment environment;

        moe::OptionSection testOpts;
        testOpts.addPositionalOption(moe::PositionalOptionDescription("positional", moe::String));

        std::vector<std::string> argv;
        argv.push_back("binaryname");
        argv.push_back("positional");
        std::map<std::string, std::string> env_map;

        moe::Value value;
        ASSERT_OK(parser.run(testOpts, argv, env_map, &environment));
        ASSERT_OK(environment.get(moe::Key("positional"), &value));
        std::string positional;
        ASSERT_OK(value.get(&positional));
        ASSERT_EQUALS(positional, "positional");
    }

    TEST(Parsing, PositionalTooMany) {
        moe::OptionsParser parser;
        moe::Environment environment;

        moe::OptionSection testOpts;
        testOpts.addPositionalOption(moe::PositionalOptionDescription("positional", moe::String));

        std::vector<std::string> argv;
        argv.push_back("binaryname");
        argv.push_back("positional");
        argv.push_back("extrapositional");
        std::map<std::string, std::string> env_map;

        moe::Value value;
        ASSERT_NOT_OK(parser.run(testOpts, argv, env_map, &environment));
    }

    TEST(Parsing, PositionalAndFlag) {
        moe::OptionsParser parser;
        moe::Environment environment;

        moe::OptionSection testOpts;
        testOpts.addPositionalOption(moe::PositionalOptionDescription("positional", moe::String));
        testOpts.addOptionChaining("port", "port", moe::Int, "Port");

        std::vector<std::string> argv;
        argv.push_back("binaryname");
        argv.push_back("positional");
        argv.push_back("--port");
        argv.push_back("5");
        std::map<std::string, std::string> env_map;

        moe::Value value;
        ASSERT_OK(parser.run(testOpts, argv, env_map, &environment));
        ASSERT_OK(environment.get(moe::Key("positional"), &value));
        std::string positional;
        ASSERT_OK(value.get(&positional));
        ASSERT_EQUALS(positional, "positional");
        ASSERT_OK(environment.get(moe::Key("port"), &value));
        int port;
        ASSERT_OK(value.get(&port));
        ASSERT_EQUALS(port, 5);
    }

    TEST(Parsing, PositionalAlreadyRegistered) {
        moe::OptionsParser parser;
        moe::Environment environment;

        moe::OptionSection testOpts;
        testOpts.addOptionChaining("positional", "positional", moe::String,
                                                  "Positional");
        testOpts.addPositionalOption(moe::PositionalOptionDescription("positional", moe::String));

        std::vector<std::string> argv;
        argv.push_back("binaryname");
        argv.push_back("positional");
        std::map<std::string, std::string> env_map;

        moe::Value value;
        ASSERT_OK(parser.run(testOpts, argv, env_map, &environment));
        ASSERT_OK(environment.get(moe::Key("positional"), &value));
        std::string positional;
        ASSERT_OK(value.get(&positional));
        ASSERT_EQUALS(positional, "positional");
    }

    TEST(Parsing, PositionalMultiple) {
        moe::OptionsParser parser;
        moe::Environment environment;

        moe::OptionSection testOpts;
        testOpts.addPositionalOption(moe::PositionalOptionDescription("positional",
                                                                    moe::StringVector, 2));

        std::vector<std::string> argv;
        argv.push_back("binaryname");
        argv.push_back("positional1");
        argv.push_back("positional2");
        std::map<std::string, std::string> env_map;

        ASSERT_OK(parser.run(testOpts, argv, env_map, &environment));
        moe::Value value;
        ASSERT_OK(environment.get(moe::Key("positional"), &value));
        std::vector<std::string> positional;
        ASSERT_OK(value.get(&positional));
        std::vector<std::string>::iterator positionalit = positional.begin();
        ASSERT_EQUALS(*positionalit, "positional1");
        positionalit++;
        ASSERT_EQUALS(*positionalit, "positional2");
    }

    TEST(Parsing, PositionalMultipleExtra) {
        moe::OptionsParser parser;
        moe::Environment environment;

        moe::OptionSection testOpts;
        testOpts.addPositionalOption(moe::PositionalOptionDescription("positional",
                                                                    moe::StringVector, 2));

        std::vector<std::string> argv;
        argv.push_back("binaryname");
        argv.push_back("positional1");
        argv.push_back("positional2");
        argv.push_back("positional2");
        std::map<std::string, std::string> env_map;

        ASSERT_NOT_OK(parser.run(testOpts, argv, env_map, &environment));
    }

    TEST(Parsing, PositionalMultipleUnlimited) {
        moe::OptionsParser parser;
        moe::Environment environment;

        moe::OptionSection testOpts;
        testOpts.addPositionalOption(moe::PositionalOptionDescription("positional",
                                                                    moe::StringVector, -1));

        std::vector<std::string> argv;
        argv.push_back("binaryname");
        argv.push_back("positional1");
        argv.push_back("positional2");
        argv.push_back("positional3");
        argv.push_back("positional4");
        argv.push_back("positional5");
        std::map<std::string, std::string> env_map;

        ASSERT_OK(parser.run(testOpts, argv, env_map, &environment));
        moe::Value value;
        ASSERT_OK(environment.get(moe::Key("positional"), &value));
        std::vector<std::string> positional;
        ASSERT_OK(value.get(&positional));
        std::vector<std::string>::iterator positionalit = positional.begin();
        ASSERT_EQUALS(*positionalit, "positional1");
        positionalit++;
        ASSERT_EQUALS(*positionalit, "positional2");
        positionalit++;
        ASSERT_EQUALS(*positionalit, "positional3");
        positionalit++;
        ASSERT_EQUALS(*positionalit, "positional4");
        positionalit++;
        ASSERT_EQUALS(*positionalit, "positional5");
    }

    TEST(Parsing, PositionalMultipleAndFlag) {
        moe::OptionsParser parser;
        moe::Environment environment;

        moe::OptionSection testOpts;
        testOpts.addPositionalOption(moe::PositionalOptionDescription("positional",
                                                                    moe::StringVector, 2));
        testOpts.addOptionChaining("port", "port", moe::Int, "Port");

        std::vector<std::string> argv;
        argv.push_back("binaryname");
        argv.push_back("positional1");
        argv.push_back("--port");
        argv.push_back("5");
        argv.push_back("positional2");
        std::map<std::string, std::string> env_map;

        ASSERT_OK(parser.run(testOpts, argv, env_map, &environment));
        moe::Value value;
        ASSERT_OK(environment.get(moe::Key("positional"), &value));
        std::vector<std::string> positional;
        ASSERT_OK(value.get(&positional));
        std::vector<std::string>::iterator positionalit = positional.begin();
        ASSERT_EQUALS(*positionalit, "positional1");
        positionalit++;
        ASSERT_EQUALS(*positionalit, "positional2");
        ASSERT_OK(environment.get(moe::Key("port"), &value));
        int port;
        ASSERT_OK(value.get(&port));
        ASSERT_EQUALS(port, 5);
    }

    TEST(Parsing, NeedArg) {
        moe::OptionsParser parser;
        moe::Environment environment;

        moe::OptionSection testOpts;
        testOpts.addOptionChaining("help", "help", moe::Switch, "Display help");
        testOpts.addOptionChaining("port", "port", moe::Int, "Port");

        std::vector<std::string> argv;
        argv.push_back("binaryname");
        argv.push_back("--port");
        std::map<std::string, std::string> env_map;

        ASSERT_NOT_OK(parser.run(testOpts, argv, env_map, &environment));
    }

    TEST(Parsing, BadArg) {
        moe::OptionsParser parser;
        moe::Environment environment;

        moe::OptionSection testOpts;
        testOpts.addOptionChaining("help", "help", moe::Switch, "Display help");
        testOpts.addOptionChaining("port", "port", moe::Int, "Port");

        std::vector<std::string> argv;
        argv.push_back("binaryname");
        argv.push_back("--port");
        argv.push_back("string");
        std::map<std::string, std::string> env_map;

        ASSERT_NOT_OK(parser.run(testOpts, argv, env_map, &environment));
    }

    TEST(Parsing, ExtraArg) {
        moe::OptionsParser parser;
        moe::Environment environment;

        moe::OptionSection testOpts;
        testOpts.addOptionChaining("help", "help", moe::Switch, "Display help");
        testOpts.addOptionChaining("port", "port", moe::Int, "Port");

        std::vector<std::string> argv;
        argv.push_back("binaryname");
        argv.push_back("--help");
        argv.push_back("string");
        std::map<std::string, std::string> env_map;

        ASSERT_NOT_OK(parser.run(testOpts, argv, env_map, &environment));
    }

    TEST(Parsing, DefaultValue) {
        moe::OptionsParser parser;
        moe::Environment environment;

        moe::OptionSection testOpts;
        testOpts.addOptionChaining("help", "help", moe::Switch, "Display help");
        testOpts.addOptionChaining("port", "port", moe::Int, "Port").setDefault(moe::Value(5));

        std::vector<std::string> argv;
        argv.push_back("binaryname");
        std::map<std::string, std::string> env_map;

        ASSERT_OK(parser.run(testOpts, argv, env_map, &environment));
        moe::Value value;
        ASSERT_OK(environment.get(moe::Key("port"), &value));
        int port;
        ASSERT_OK(value.get(&port));
        ASSERT_EQUALS(port, 5);
    }

    TEST(Parsing, DefaultValueOverride) {
        moe::OptionsParser parser;
        moe::Environment environment;

        moe::OptionSection testOpts;
        testOpts.addOptionChaining("help", "help", moe::Switch, "Display help");
        testOpts.addOptionChaining("port", "port", moe::Int, "Port").setDefault(moe::Value(5));

        std::vector<std::string> argv;
        argv.push_back("binaryname");
        argv.push_back("--port");
        argv.push_back("6");
        std::map<std::string, std::string> env_map;

        ASSERT_OK(parser.run(testOpts, argv, env_map, &environment));
        moe::Value value;
        ASSERT_OK(environment.get(moe::Key("port"), &value));
        int port;
        ASSERT_OK(value.get(&port));
        ASSERT_EQUALS(port, 6);
    }

    TEST(Parsing, DefaultValuesNotInBSON) {
        moe::OptionsParser parser;
        moe::Environment environment;

        moe::OptionSection testOpts;
        testOpts.addOptionChaining("help", "help", moe::Switch, "Display help");
        testOpts.addOptionChaining("val1", "val1", moe::Int, "Val1").setDefault(moe::Value(5));
        testOpts.addOptionChaining("val2", "val2", moe::Int, "Val2").setDefault(moe::Value(5));

        std::vector<std::string> argv;
        argv.push_back("binaryname");
        argv.push_back("--val1");
        argv.push_back("6");
        std::map<std::string, std::string> env_map;

        ASSERT_OK(parser.run(testOpts, argv, env_map, &environment));

        mongo::BSONObj expected = BSON("val1" << 6);
        ASSERT_EQUALS(expected, environment.toBSON());
    }

    TEST(Parsing, ImplicitValue) {
        moe::OptionsParser parser;
        moe::Environment environment;

        moe::OptionSection testOpts;
        testOpts.addOptionChaining("help", "help", moe::Switch, "Display help");
        testOpts.addOptionChaining("port", "port", moe::Int, "Port")
                                  .setDefault(moe::Value(6)).setImplicit(moe::Value(7));

        std::vector<std::string> argv;
        argv.push_back("binaryname");
        argv.push_back("--port");
        std::map<std::string, std::string> env_map;

        ASSERT_OK(parser.run(testOpts, argv, env_map, &environment));
        moe::Value value;
        ASSERT_OK(environment.get(moe::Key("port"), &value));
        int port;
        ASSERT_OK(value.get(&port));
        ASSERT_EQUALS(port, 7);
    }

    TEST(Parsing, ImplicitValueDefault) {
        moe::OptionsParser parser;
        moe::Environment environment;

        moe::OptionSection testOpts;
        testOpts.addOptionChaining("help", "help", moe::Switch, "Display help");
        testOpts.addOptionChaining("port", "port", moe::Int, "Port")
                                  .setDefault(moe::Value(6)).setImplicit(moe::Value(7));

        std::vector<std::string> argv;
        argv.push_back("binaryname");
        std::map<std::string, std::string> env_map;

        ASSERT_OK(parser.run(testOpts, argv, env_map, &environment));
        moe::Value value;
        ASSERT_OK(environment.get(moe::Key("port"), &value));
        int port;
        ASSERT_OK(value.get(&port));
        ASSERT_EQUALS(port, 6);
    }

    TEST(Parsing, ImplicitValueOverride) {
        moe::OptionsParser parser;
        moe::Environment environment;

        moe::OptionSection testOpts;
        testOpts.addOptionChaining("help", "help", moe::Switch, "Display help");
        testOpts.addOptionChaining("port", "port", moe::Int, "Port")
                                  .setDefault(moe::Value(6)).setImplicit(moe::Value(7));

        std::vector<std::string> argv;
        argv.push_back("binaryname");
        argv.push_back("--port");
        argv.push_back("5");
        std::map<std::string, std::string> env_map;

        ASSERT_OK(parser.run(testOpts, argv, env_map, &environment));
        moe::Value value;
        ASSERT_OK(environment.get(moe::Key("port"), &value));
        int port;
        ASSERT_OK(value.get(&port));
        ASSERT_EQUALS(port, 5);
    }

    TEST(Parsing, ShortName) {
        moe::OptionsParser parser;
        moe::Environment environment;

        moe::OptionSection testOpts;
        testOpts.addOptionChaining("help", "help,h", moe::Switch, "Display help");
        testOpts.addOptionChaining("port", "port,p", moe::Int, "Port");

        std::vector<std::string> argv;
        argv.push_back("binaryname");
        argv.push_back("-p");
        argv.push_back("5");
        argv.push_back("-h");
        std::map<std::string, std::string> env_map;

        moe::Value value;
        ASSERT_OK(parser.run(testOpts, argv, env_map, &environment));
        ASSERT_OK(environment.get(moe::Key("help"), &value));
        ASSERT_OK(environment.get(moe::Key("port"), &value));
        int port;
        ASSERT_OK(value.get(&port));
        ASSERT_EQUALS(port, 5);
    }

    TEST(Style, NoSticky) {
        moe::OptionsParser parser;
        moe::Environment environment;

        moe::OptionSection testOpts;
        testOpts.addOptionChaining("opt", "opt,o", moe::Switch, "first opt");
        testOpts.addOptionChaining("arg", "arg,a", moe::Switch, "first arg");

        std::vector<std::string> argv;
        argv.push_back("binaryname");
        argv.push_back("-oa");
        std::map<std::string, std::string> env_map;

        ASSERT_NOT_OK(parser.run(testOpts, argv, env_map, &environment));
    }

    TEST(Style, NoGuessing) {
        moe::OptionsParser parser;
        moe::Environment environment;

        moe::OptionSection testOpts;
        testOpts.addOptionChaining("help", "help", moe::Switch, "Display help");

        std::vector<std::string> argv;
        argv.push_back("binaryname");
        argv.push_back("--hel");
        std::map<std::string, std::string> env_map;

        ASSERT_NOT_OK(parser.run(testOpts, argv, env_map, &environment));
    }

    TEST(Style, LongDisguises) {
        moe::OptionsParser parser;
        moe::Environment environment;

        moe::OptionSection testOpts;
        testOpts.addOptionChaining("help", "help", moe::Switch, "Display help");

        std::vector<std::string> argv;
        argv.push_back("binaryname");
        argv.push_back("-help");
        std::map<std::string, std::string> env_map;

        ASSERT_OK(parser.run(testOpts, argv, env_map, &environment));
        moe::Value value;
        ASSERT_OK(environment.get(moe::Key("help"), &value));
        bool help;
        ASSERT_OK(value.get(&help));
        ASSERT_EQUALS(help, true);
    }

    TEST(Style, Verbosity) {
        moe::OptionsParser parser;
        moe::Environment environment;

        moe::OptionSection testOpts;
        testOpts.addOptionChaining("v", "verbose,v", moe::Switch,
                        "be more verbose (include multiple times for more verbosity e.g. -vvvvv)");

        /* support for -vv -vvvv etc. */
        for (std::string s = "vv"; s.length() <= 12; s.append("v")) {
            testOpts.addOptionChaining(s.c_str(), s.c_str(), moe::Switch,
                                       "higher verbosity levels (hidden)").hidden();
        }

        std::vector<std::string> argv;
        argv.push_back("binaryname");
        argv.push_back("-vvvvvv");
        std::map<std::string, std::string> env_map;

        ASSERT_OK(parser.run(testOpts, argv, env_map, &environment));

        moe::Value value;
        for (std::string s = "vv"; s.length() <= 12; s.append("v")) {
            if (s.length() == 6) {
                ASSERT_OK(environment.get(moe::Key(s), &value));
                bool verbose;
                ASSERT_OK(value.get(&verbose));
                ASSERT_EQUALS(verbose, true);
            }
            else {
                ASSERT_NOT_OK(environment.get(moe::Key(s), &value));
            }
        }
    }

    TEST(INIConfigFile, Basic) {
        OptionsParserTester parser;
        moe::Environment environment;

        moe::OptionSection testOpts;
        testOpts.addOptionChaining("config", "config", moe::String, "Config file to parse");
        testOpts.addOptionChaining("port", "port", moe::Int, "Port");

        std::vector<std::string> argv;
        argv.push_back("binaryname");
        argv.push_back("--config");
        argv.push_back("default.conf");
        std::map<std::string, std::string> env_map;

        parser.setConfig("default.conf", "port=5");

        ASSERT_OK(parser.run(testOpts, argv, env_map, &environment));
        moe::Value value;
        ASSERT_OK(environment.get(moe::Key("port"), &value));
        int port;
        ASSERT_OK(value.get(&port));
        ASSERT_EQUALS(port, 5);
    }

    TEST(INIConfigFile, Empty) {
        OptionsParserTester parser;
        moe::Environment environment;

        moe::OptionSection testOpts;
        testOpts.addOptionChaining("config", "config", moe::String, "Config file to parse");

        std::vector<std::string> argv;
        argv.push_back("binaryname");
        argv.push_back("--config");
        argv.push_back("default.conf");
        std::map<std::string, std::string> env_map;

        parser.setConfig("default.conf", "");

        ASSERT_OK(parser.run(testOpts, argv, env_map, &environment));
    }

    TEST(INIConfigFile, Override) {
        OptionsParserTester parser;
        moe::Environment environment;

        moe::OptionSection testOpts;
        testOpts.addOptionChaining("config", "config", moe::String, "Config file to parse");
        testOpts.addOptionChaining("port", "port", moe::Int, "Port");

        std::vector<std::string> argv;
        argv.push_back("binaryname");
        argv.push_back("--config");
        argv.push_back("default.conf");
        argv.push_back("--port");
        argv.push_back("6");
        std::map<std::string, std::string> env_map;

        parser.setConfig("default.conf", "port=5");

        ASSERT_OK(parser.run(testOpts, argv, env_map, &environment));
        moe::Value value;
        ASSERT_OK(environment.get(moe::Key("port"), &value));
        int port;
        ASSERT_OK(value.get(&port));
        ASSERT_EQUALS(port, 6);
    }

    TEST(INIConfigFile, Comments) {
        OptionsParserTester parser;
        moe::Environment environment;

        moe::OptionSection testOpts;
        testOpts.addOptionChaining("config", "config", moe::String, "Config file to parse");
        testOpts.addOptionChaining("port", "port", moe::Int, "Port");
        testOpts.addOptionChaining("str", "str", moe::String, "String");

        std::vector<std::string> argv;
        argv.push_back("binaryname");
        argv.push_back("--config");
        argv.push_back("default.conf");
        std::map<std::string, std::string> env_map;

        parser.setConfig("default.conf", "# port=5\nstr=NotCommented");

        ASSERT_OK(parser.run(testOpts, argv, env_map, &environment));
        moe::Value value;
        ASSERT_NOT_OK(environment.get(moe::Key("port"), &value));
        ASSERT_OK(environment.get(moe::Key("str"), &value));
        std::string str;
        ASSERT_OK(value.get(&str));
        ASSERT_EQUALS(str, "NotCommented");
    }

    TEST(INIConfigFile, Monkeys) {
        OptionsParserTester parser;
        moe::Environment environment;

        moe::OptionSection testOpts;
        testOpts.addOptionChaining("config", "config", moe::String, "Config file to parse");
        testOpts.addOptionChaining("this", "this", moe::Switch, "This");
        testOpts.addOptionChaining("that", "that", moe::Switch, "That");
        testOpts.addOptionChaining("another", "another", moe::String, "Another");
        testOpts.addOptionChaining("other", "other", moe::String, "Other");

        std::vector<std::string> argv;
        argv.push_back("binaryname");
        argv.push_back("--config");
        argv.push_back("default.conf");
        std::map<std::string, std::string> env_map;

        parser.setConfig("default.conf",
                         "\t this = false \n#that = true\n #another = whocares"
                         "\n\n other = monkeys ");

        ASSERT_OK(parser.run(testOpts, argv, env_map, &environment));
        moe::Value value;
        ASSERT_NOT_OK(environment.get(moe::Key("this"), &value));
        ASSERT_NOT_OK(environment.get(moe::Key("that"), &value));
        ASSERT_NOT_OK(environment.get(moe::Key("another"), &value));
        ASSERT_OK(environment.get(moe::Key("other"), &value));
        std::string str;
        ASSERT_OK(value.get(&str));
        ASSERT_EQUALS(str, "monkeys");
    }

    TEST(INIConfigFile, DefaultValueOverride) {
        OptionsParserTester parser;
        moe::Environment environment;

        moe::OptionSection testOpts;
        testOpts.addOptionChaining("config", "config", moe::String, "Config file to parse");
        testOpts.addOptionChaining("port", "port", moe::Int, "Port").setDefault(moe::Value(5));

        std::vector<std::string> argv;
        argv.push_back("binaryname");
        argv.push_back("--config");
        argv.push_back("default.conf");
        std::map<std::string, std::string> env_map;

        parser.setConfig("default.conf", "port=6");

        ASSERT_OK(parser.run(testOpts, argv, env_map, &environment));
        moe::Value value;
        ASSERT_OK(environment.get(moe::Key("port"), &value));
        int port;
        ASSERT_OK(value.get(&port));
        ASSERT_EQUALS(port, 6);
    }

    TEST(JSONConfigFile, Basic) {
        OptionsParserTester parser;
        moe::Environment environment;

        moe::OptionSection testOpts;
        testOpts.addOptionChaining("config", "config", moe::String, "Config file to parse");
        testOpts.addOptionChaining("port", "port", moe::Int, "Port");

        std::vector<std::string> argv;
        argv.push_back("binaryname");
        argv.push_back("--config");
        argv.push_back("config.json");
        std::map<std::string, std::string> env_map;

        parser.setConfig("config.json", "{ port : 5 }");

        ASSERT_OK(parser.run(testOpts, argv, env_map, &environment));
        moe::Value value;
        ASSERT_OK(environment.get(moe::Key("port"), &value));
        int port;
        ASSERT_OK(value.get(&port));
        ASSERT_EQUALS(port, 5);
    }

    TEST(JSONConfigFile, Empty) {
        OptionsParserTester parser;
        moe::Environment environment;

        moe::OptionSection testOpts;
        testOpts.addOptionChaining("config", "config", moe::String, "Config file to parse");

        std::vector<std::string> argv;
        argv.push_back("binaryname");
        argv.push_back("--config");
        argv.push_back("config.json");
        std::map<std::string, std::string> env_map;

        parser.setConfig("config.json", "");

        ASSERT_OK(parser.run(testOpts, argv, env_map, &environment));
    }

    TEST(JSONConfigFile, EmptyObject) {
        OptionsParserTester parser;
        moe::Environment environment;

        moe::OptionSection testOpts;
        testOpts.addOptionChaining("config", "config", moe::String, "Config file to parse");

        std::vector<std::string> argv;
        argv.push_back("binaryname");
        argv.push_back("--config");
        argv.push_back("config.json");
        std::map<std::string, std::string> env_map;

        parser.setConfig("config.json", "{}");

        ASSERT_OK(parser.run(testOpts, argv, env_map, &environment));
    }

    TEST(JSONConfigFile, Override) {
        OptionsParserTester parser;
        moe::Environment environment;

        moe::OptionSection testOpts;
        testOpts.addOptionChaining("config", "config", moe::String, "Config file to parse");
        testOpts.addOptionChaining("port", "port", moe::Int, "Port");

        std::vector<std::string> argv;
        argv.push_back("binaryname");
        argv.push_back("--config");
        argv.push_back("config.json");
        argv.push_back("--port");
        argv.push_back("6");
        std::map<std::string, std::string> env_map;


        parser.setConfig("config.json", "{ port : 5 }");

        ASSERT_OK(parser.run(testOpts, argv, env_map, &environment));
        moe::Value value;
        ASSERT_OK(environment.get(moe::Key("port"), &value));
        int port;
        ASSERT_OK(value.get(&port));
        ASSERT_EQUALS(port, 6);
    }

    TEST(JSONConfigFile, UnregisteredOption) {
        OptionsParserTester parser;
        moe::Environment environment;

        moe::OptionSection testOpts;
        testOpts.addOptionChaining("config", "config", moe::String, "Config file to parse");

        std::vector<std::string> argv;
        argv.push_back("binaryname");
        argv.push_back("--config");
        argv.push_back("config.json");
        std::map<std::string, std::string> env_map;

        parser.setConfig("config.json", "{ port : 5 }");

        ASSERT_NOT_OK(parser.run(testOpts, argv, env_map, &environment));
    }

    TEST(JSONConfigFile, DuplicateOption) {
        OptionsParserTester parser;
        moe::Environment environment;

        moe::OptionSection testOpts;
        testOpts.addOptionChaining("config", "config", moe::String, "Config file to parse");
        testOpts.addOptionChaining("port", "port", moe::Int, "Port");

        std::vector<std::string> argv;
        argv.push_back("binaryname");
        argv.push_back("--config");
        argv.push_back("config.json");
        std::map<std::string, std::string> env_map;

        parser.setConfig("config.json", "{ port : 5, port : 5 }");

        ASSERT_NOT_OK(parser.run(testOpts, argv, env_map, &environment));
    }

    TEST(JSONConfigFile, BadType) {
        OptionsParserTester parser;
        moe::Environment environment;

        moe::OptionSection testOpts;
        testOpts.addOptionChaining("config", "config", moe::String, "Config file to parse");
        testOpts.addOptionChaining("port", "port", moe::Int, "Port");

        std::vector<std::string> argv;
        argv.push_back("binaryname");
        argv.push_back("--config");
        argv.push_back("config.json");
        std::map<std::string, std::string> env_map;

        parser.setConfig("config.json", "{ port : \"string\" }");

        ASSERT_NOT_OK(parser.run(testOpts, argv, env_map, &environment));
    }

    TEST(JSONConfigFile, Nested) {
        OptionsParserTester parser;
        moe::Environment environment;

        moe::OptionSection testOpts;
        testOpts.addOptionChaining("config", "config", moe::String, "Config file to parse");
        testOpts.addOptionChaining("nested.port", "port", moe::Int, "Port");

        std::vector<std::string> argv;
        argv.push_back("binaryname");
        argv.push_back("--config");
        argv.push_back("config.json");
        std::map<std::string, std::string> env_map;

        parser.setConfig("config.json", "{ nested : { port : 5 } }");

        ASSERT_OK(parser.run(testOpts, argv, env_map, &environment));
        moe::Value value;
        ASSERT_OK(environment.get(moe::Key("nested.port"), &value));
        int port;
        ASSERT_OK(value.get(&port));
        ASSERT_EQUALS(port, 5);
    }

    TEST(JSONConfigFile, Dotted) {
        OptionsParserTester parser;
        moe::Environment environment;

        moe::OptionSection testOpts;
        testOpts.addOptionChaining("config", "config", moe::String, "Config file to parse");
        testOpts.addOptionChaining("dotted.port", "port", moe::Int, "Port");

        std::vector<std::string> argv;
        argv.push_back("binaryname");
        argv.push_back("--config");
        argv.push_back("config.json");
        std::map<std::string, std::string> env_map;

        parser.setConfig("config.json", "{ \"dotted.port\" : 5 }");

        ASSERT_OK(parser.run(testOpts, argv, env_map, &environment));
        moe::Value value;
        ASSERT_OK(environment.get(moe::Key("dotted.port"), &value));
        int port;
        ASSERT_OK(value.get(&port));
        ASSERT_EQUALS(port, 5);
    }

    TEST(JSONConfigFile, DottedAndNested) {
        OptionsParserTester parser;
        moe::Environment environment;

        moe::OptionSection testOpts;
        testOpts.addOptionChaining("config", "config", moe::String, "Config file to parse");
        testOpts.addOptionChaining("dottednested.var1", "var1", moe::Int, "Var1");
        testOpts.addOptionChaining("dottednested.var2", "var2", moe::Int, "Var2");

        std::vector<std::string> argv;
        argv.push_back("binaryname");
        argv.push_back("--config");
        argv.push_back("config.json");
        std::map<std::string, std::string> env_map;

        parser.setConfig("config.json",
                         "{ \"dottednested.var1\" : 5, dottednested : { var2 : 6 } }");

        ASSERT_OK(parser.run(testOpts, argv, env_map, &environment));
        moe::Value value;
        ASSERT_OK(environment.get(moe::Key("dottednested.var1"), &value));
        int var1;
        ASSERT_OK(value.get(&var1));
        ASSERT_EQUALS(var1, 5);
        ASSERT_OK(environment.get(moe::Key("dottednested.var2"), &value));
        int var2;
        ASSERT_OK(value.get(&var2));
        ASSERT_EQUALS(var2, 6);
    }

    TEST(JSONConfigFile, StringVector) {
        OptionsParserTester parser;
        moe::Environment environment;

        moe::OptionSection testOpts;
        testOpts.addOptionChaining("config", "config", moe::String, "Config file to parse");
        testOpts.addOptionChaining("multival", "multival", moe::StringVector, "Multiple Values");

        std::vector<std::string> argv;
        argv.push_back("binaryname");
        argv.push_back("--config");
        argv.push_back("config.json");
        std::map<std::string, std::string> env_map;

        parser.setConfig("config.json", "{ multival : [ \"val1\", \"val2\" ] }");

        moe::Value value;
        ASSERT_OK(parser.run(testOpts, argv, env_map, &environment));
        ASSERT_OK(environment.get(moe::Key("multival"), &value));
        std::vector<std::string> multival;
        std::vector<std::string>::iterator multivalit;
        ASSERT_OK(value.get(&multival));
        multivalit = multival.begin();
        ASSERT_EQUALS(*multivalit, "val1");
        multivalit++;
        ASSERT_EQUALS(*multivalit, "val2");
    }

    TEST(JSONConfigFile, StringVectorNonString) {
        OptionsParserTester parser;
        moe::Environment environment;

        moe::OptionSection testOpts;
        testOpts.addOptionChaining("config", "config", moe::String, "Config file to parse");
        testOpts.addOptionChaining("multival", "multival", moe::StringVector, "Multiple Values");

        std::vector<std::string> argv;
        argv.push_back("binaryname");
        argv.push_back("--config");
        argv.push_back("config.json");
        std::map<std::string, std::string> env_map;

        parser.setConfig("config.json", "{ multival : [ 1 ] }");

        ASSERT_NOT_OK(parser.run(testOpts, argv, env_map, &environment));
    }

    TEST(JSONConfigFile, Over16Megabytes) {
        // Test to make sure that we fail gracefully when we try to parse a JSON config file that
        // results in a BSON object larger than the current limit of 16MB
        OptionsParserTester parser;
        moe::Environment environment;

        moe::OptionSection testOpts;
        testOpts.addOptionChaining("config", "config", moe::String, "Config file to parse");

        std::vector<std::string> argv;
        argv.push_back("binaryname");
        argv.push_back("--config");
        argv.push_back("config.json");
        std::map<std::string, std::string> env_map;

        // 1024 characters = 64 * 16
        const std::string largeString =
            "\""
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
            "\"";

        std::string largeConfigString;

        largeConfigString.append("{ \"largeArray\" : [ ");

        // 16mb = 16 * 1024kb = 16384
        for (int i = 0; i < 16383; i++) {
            largeConfigString.append(largeString);
            largeConfigString.append(",");
        }
        largeConfigString.append(largeString);
        largeConfigString.append(" ] }");

        parser.setConfig("config.json", largeConfigString);

        moe::Value value;
        ASSERT_NOT_OK(parser.run(testOpts, argv, env_map, &environment));
    }

    TEST(JSONConfigFile, DefaultValueOverride) {
        OptionsParserTester parser;
        moe::Environment environment;

        moe::OptionSection testOpts;
        testOpts.addOptionChaining("config", "config", moe::String, "Config file to parse");
        testOpts.addOptionChaining("port", "port", moe::Int, "Port").setDefault(moe::Value(5));

        std::vector<std::string> argv;
        argv.push_back("binaryname");
        argv.push_back("--config");
        argv.push_back("config.json");
        std::map<std::string, std::string> env_map;

        parser.setConfig("config.json", "{ port : 6 }");

        ASSERT_OK(parser.run(testOpts, argv, env_map, &environment));
        moe::Value value;
        ASSERT_OK(environment.get(moe::Key("port"), &value));
        int port;
        ASSERT_OK(value.get(&port));
        ASSERT_EQUALS(port, 6);
    }

    TEST(Parsing, BadConfigFileOption) {
        OptionsParserTester parser;
        moe::Environment environment;

        moe::OptionSection testOpts;

        // TODO: Should the error be in here?
        testOpts.addOptionChaining("config", "config", moe::Int, "Config file to parse");

        std::vector<std::string> argv;
        argv.push_back("binaryname");
        argv.push_back("--config");
        argv.push_back("1");
        std::map<std::string, std::string> env_map;

        parser.setConfig("default.conf", "");

        ASSERT_NOT_OK(parser.run(testOpts, argv, env_map, &environment));
    }

    TEST(ConfigFromFilesystem, JSONGood) {
        moe::OptionsParser parser;
        moe::Environment environment;

        moe::OptionSection testOpts;
        testOpts.addOptionChaining("config", "config", moe::String, "Config file to parse");
        testOpts.addOptionChaining("port", "port", moe::Int, "Port");

        std::vector<std::string> argv;
        argv.push_back("binaryname");
        argv.push_back("--config");
        argv.push_back(TEST_CONFIG_PATH("good.json"));
        std::map<std::string, std::string> env_map;

        moe::Value value;
        ASSERT_OK(parser.run(testOpts, argv, env_map, &environment));
        ASSERT_OK(environment.get(moe::Key("port"), &value));
        int port;
        ASSERT_OK(value.get(&port));
        ASSERT_EQUALS(port, 5);
    }

    TEST(ConfigFromFilesystem, INIGood) {
        moe::OptionsParser parser;
        moe::Environment environment;

        moe::OptionSection testOpts;
        testOpts.addOptionChaining("config", "config", moe::String, "Config file to parse");
        testOpts.addOptionChaining("port", "port", moe::Int, "Port");

        std::vector<std::string> argv;
        argv.push_back("binaryname");
        argv.push_back("--config");
        argv.push_back(TEST_CONFIG_PATH("good.conf"));
        std::map<std::string, std::string> env_map;

        moe::Value value;
        ASSERT_OK(parser.run(testOpts, argv, env_map, &environment));
        ASSERT_OK(environment.get(moe::Key("port"), &value));
        int port;
        ASSERT_OK(value.get(&port));
        ASSERT_EQUALS(port, 5);
    }

    TEST(ConfigFromFilesystem, Empty) {
        moe::OptionsParser parser;
        moe::Environment environment;

        moe::OptionSection testOpts;
        testOpts.addOptionChaining("config", "config", moe::String, "Config file to parse");

        std::vector<std::string> argv;
        argv.push_back("binaryname");
        argv.push_back("--config");
        argv.push_back(TEST_CONFIG_PATH("empty.json"));
        std::map<std::string, std::string> env_map;

        moe::Value value;
        ASSERT_OK(parser.run(testOpts, argv, env_map, &environment));
    }

    TEST(JSONConfigFile, Composing) {
        OptionsParserTester parser;
        moe::Environment environment;

        moe::OptionSection testOpts;
        testOpts.addOptionChaining("config", "config", moe::String, "Config file to parse");
        testOpts.addOptionChaining("setParameter", "setParameter", moe::StringVector,
                                                  "Multiple Values").composing();

        std::vector<std::string> argv;
        argv.push_back("binaryname");
        argv.push_back("--config");
        argv.push_back("config.json");
        argv.push_back("--setParameter");
        argv.push_back("val1");
        argv.push_back("--setParameter");
        argv.push_back("val2");
        std::map<std::string, std::string> env_map;

        parser.setConfig("config.json", "{ setParameter : [ \"val3\", \"val4\" ] }");

        moe::Value value;
        ASSERT_OK(parser.run(testOpts, argv, env_map, &environment));
        ASSERT_OK(environment.get(moe::Key("setParameter"), &value));
        std::vector<std::string> setParameter;
        std::vector<std::string>::iterator setParameterit;
        ASSERT_OK(value.get(&setParameter));
        ASSERT_EQUALS(setParameter.size(), static_cast<size_t>(4));
        setParameterit = setParameter.begin();
        ASSERT_EQUALS(*setParameterit, "val1");
        setParameterit++;
        ASSERT_EQUALS(*setParameterit, "val2");
        setParameterit++;
        ASSERT_EQUALS(*setParameterit, "val3");
        setParameterit++;
        ASSERT_EQUALS(*setParameterit, "val4");
    }

    TEST(INIConfigFile, Composing) {
        OptionsParserTester parser;
        moe::Environment environment;

        moe::OptionSection testOpts;
        testOpts.addOptionChaining("config", "config", moe::String, "Config file to parse");
        testOpts.addOptionChaining("setParameter", "setParameter", moe::StringVector,
                                   "Multiple Values").composing();

        std::vector<std::string> argv;
        argv.push_back("binaryname");
        argv.push_back("--config");
        argv.push_back("default.conf");
        argv.push_back("--setParameter");
        argv.push_back("val1");
        argv.push_back("--setParameter");
        argv.push_back("val2");
        std::map<std::string, std::string> env_map;

        parser.setConfig("default.conf", "setParameter=val3\nsetParameter=val4");

        moe::Value value;
        ASSERT_OK(parser.run(testOpts, argv, env_map, &environment));
        ASSERT_OK(environment.get(moe::Key("setParameter"), &value));
        std::vector<std::string> setParameter;
        std::vector<std::string>::iterator setParameterit;
        ASSERT_OK(value.get(&setParameter));
        ASSERT_EQUALS(setParameter.size(), static_cast<size_t>(4));
        setParameterit = setParameter.begin();
        ASSERT_EQUALS(*setParameterit, "val1");
        setParameterit++;
        ASSERT_EQUALS(*setParameterit, "val2");
        setParameterit++;
        ASSERT_EQUALS(*setParameterit, "val3");
        setParameterit++;
        ASSERT_EQUALS(*setParameterit, "val4");
    }

    TEST(LegacyInterface, Good) {
        moe::OptionsParser parser;
        moe::Environment environment;

        moe::OptionSection testOpts;
        testOpts.addOptionChaining("port", "port", moe::Int, "Port");

        std::vector<std::string> argv;
        argv.push_back("binaryname");
        argv.push_back("--port");
        argv.push_back("5");
        std::map<std::string, std::string> env_map;

        ASSERT_OK(parser.run(testOpts, argv, env_map, &environment));
        ASSERT_TRUE(environment.count("port"));
        int port;
        try {
            port = environment["port"].as<int>();
        }
        catch ( std::exception &e ) {
            FAIL(e.what());
        }
        ASSERT_EQUALS(port, 5);
    }

    TEST(LegacyInterface, NotSpecified) {
        moe::OptionsParser parser;
        moe::Environment environment;

        moe::OptionSection testOpts;
        testOpts.addOptionChaining("port", "port", moe::Int, "Port");

        std::vector<std::string> argv;
        argv.push_back("binaryname");
        std::map<std::string, std::string> env_map;

        ASSERT_OK(parser.run(testOpts, argv, env_map, &environment));
        ASSERT_FALSE(environment.count("port"));
    }

    TEST(LegacyInterface, BadType) {
        moe::OptionsParser parser;
        moe::Environment environment;

        moe::OptionSection testOpts;
        testOpts.addOptionChaining("port", "port", moe::Int, "Port");

        std::vector<std::string> argv;
        argv.push_back("binaryname");
        argv.push_back("--port");
        argv.push_back("5");
        std::map<std::string, std::string> env_map;

        ASSERT_OK(parser.run(testOpts, argv, env_map, &environment));
        ASSERT_TRUE(environment.count("port"));
        std::string port;
        try {
            port = environment["port"].as<std::string>();
            FAIL("Expected exception trying to convert int to type string");
        }
        catch ( std::exception &e ) {
        }
    }

    TEST(JSONConfigFile, NestedComments) {
        OptionsParserTester parser;
        moe::Environment environment;

        moe::OptionSection testOpts;
        testOpts.addOptionChaining("config", "config", moe::String, "Config file to parse");
        testOpts.addOptionChaining("port", "port", moe::Int, "Port");
        testOpts.addOptionChaining("host", "host", moe::String, "Host");

        std::vector<std::string> argv;
        argv.push_back("binaryname");
        argv.push_back("--config");
        argv.push_back("config.json");
        std::map<std::string, std::string> env_map;

        parser.setConfig("config.json",
                         "{ port : { comment : \"comment on port\", value : 5 },"
                         "host : { comment : \"comment on host\", value : \"localhost\" } }");

        moe::Value value;
        ASSERT_OK(parser.run(testOpts, argv, env_map, &environment));
        ASSERT_OK(environment.get(moe::Key("port"), &value));
        int port;
        ASSERT_OK(value.get(&port));
        ASSERT_EQUALS(port, 5);
        ASSERT_OK(environment.get(moe::Key("host"), &value));
        std::string host;
        ASSERT_OK(value.get(&host));
        ASSERT_EQUALS(host, "localhost");
    }

    TEST(JSONConfigFile, FlatComments) {
        OptionsParserTester parser;
        moe::Environment environment;

        moe::OptionSection testOpts;
        testOpts.addOptionChaining("config", "config", moe::String, "Config file to parse");
        testOpts.addOptionChaining("port", "port", moe::Int, "Port");
        testOpts.addOptionChaining("host", "host", moe::String, "Host");

        std::vector<std::string> argv;
        argv.push_back("binaryname");
        argv.push_back("--config");
        argv.push_back("config.json");
        std::map<std::string, std::string> env_map;

        parser.setConfig("config.json",
                         "{ comment : \"comment on port\", port : 5 ,"
                         "  comment : \"comment on host\", host : \"localhost\" }");

        moe::Value value;
        ASSERT_OK(parser.run(testOpts, argv, env_map, &environment));
        ASSERT_OK(environment.get(moe::Key("port"), &value));
        int port;
        ASSERT_OK(value.get(&port));
        ASSERT_EQUALS(port, 5);
        ASSERT_OK(environment.get(moe::Key("host"), &value));
        std::string host;
        ASSERT_OK(value.get(&host));
        ASSERT_EQUALS(host, "localhost");
    }

    TEST(JSONConfigFile, MixedComments) {
        OptionsParserTester parser;
        moe::Environment environment;

        moe::OptionSection testOpts;
        testOpts.addOptionChaining("config", "config", moe::String, "Config file to parse");
        testOpts.addOptionChaining("port", "port", moe::Int, "Port");
        testOpts.addOptionChaining("host", "host", moe::String, "Host");

        std::vector<std::string> argv;
        argv.push_back("binaryname");
        argv.push_back("--config");
        argv.push_back("config.json");
        std::map<std::string, std::string> env_map;

        parser.setConfig("config.json",
                         "{ port : { comment : \"comment on port\", value : 5 },"
                         "  comment : \"comment on host\", host : \"localhost\" }");

        moe::Value value;
        ASSERT_OK(parser.run(testOpts, argv, env_map, &environment));
        ASSERT_OK(environment.get(moe::Key("port"), &value));
        int port;
        ASSERT_OK(value.get(&port));
        ASSERT_EQUALS(port, 5);
        ASSERT_OK(environment.get(moe::Key("host"), &value));
        std::string host;
        ASSERT_OK(value.get(&host));
        ASSERT_EQUALS(host, "localhost");
    }

    TEST(JSONConfigFile, NestedCommentsBadValue) {
        OptionsParserTester parser;
        moe::Environment environment;

        moe::OptionSection testOpts;
        testOpts.addOptionChaining("config", "config", moe::String, "Config file to parse");
        testOpts.addOptionChaining("port", "port", moe::Int, "Port");
        testOpts.addOptionChaining("host", "host", moe::String, "Host");

        std::vector<std::string> argv;
        argv.push_back("binaryname");
        argv.push_back("--config");
        argv.push_back("config.json");
        std::map<std::string, std::string> env_map;

        parser.setConfig("config.json",
                         "{ port : { comment : \"comment on port\", value : \"string\" },"
                         "host : { comment : \"comment on host\", value : \"localhost\" } }");

        moe::Value value;
        ASSERT_NOT_OK(parser.run(testOpts, argv, env_map, &environment));
    }

    TEST(JSONConfigFile, FlatCommentsBadValue) {
        OptionsParserTester parser;
        moe::Environment environment;

        moe::OptionSection testOpts;
        testOpts.addOptionChaining("config", "config", moe::String, "Config file to parse");
        testOpts.addOptionChaining("port", "port", moe::Int, "Port");
        testOpts.addOptionChaining("host", "host", moe::String, "Host");

        std::vector<std::string> argv;
        argv.push_back("binaryname");
        argv.push_back("--config");
        argv.push_back("config.json");
        std::map<std::string, std::string> env_map;

        parser.setConfig("config.json",
                         "{ comment : \"comment on port\", port : \"string\" ,"
                         "  comment : \"comment on host\", host : \"localhost\" }");

        moe::Value value;
        ASSERT_NOT_OK(parser.run(testOpts, argv, env_map, &environment));
    }

    TEST(JSONConfigFile, NestedCommentsOtherTypes) {
        OptionsParserTester parser;
        moe::Environment environment;

        moe::OptionSection testOpts;
        testOpts.addOptionChaining("config", "config", moe::String, "Config file to parse");
        testOpts.addOptionChaining("port", "port", moe::Int, "Port");
        testOpts.addOptionChaining("host", "host", moe::String, "Host");

        std::vector<std::string> argv;
        argv.push_back("binaryname");
        argv.push_back("--config");
        argv.push_back("config.json");
        std::map<std::string, std::string> env_map;

        parser.setConfig("config.json",
                         "{ port : { comment : [ \"can\", \"be\", \"array\", true ], value : 5 },"
                         "  host : { comment : { nestedcomment : \"really descriptive\" },"
                                               " value : \"localhost\" } }");

        moe::Value value;
        ASSERT_OK(parser.run(testOpts, argv, env_map, &environment));
        ASSERT_OK(environment.get(moe::Key("port"), &value));
        int port;
        ASSERT_OK(value.get(&port));
        ASSERT_EQUALS(port, 5);
        ASSERT_OK(environment.get(moe::Key("host"), &value));
        std::string host;
        ASSERT_OK(value.get(&host));
        ASSERT_EQUALS(host, "localhost");
    }

    TEST(JSONConfigFile, FlatCommentsOtherTypes) {
        OptionsParserTester parser;
        moe::Environment environment;

        moe::OptionSection testOpts;
        testOpts.addOptionChaining("config", "config", moe::String, "Config file to parse");
        testOpts.addOptionChaining("port", "port", moe::Int, "Port");
        testOpts.addOptionChaining("host", "host", moe::String, "Host");

        std::vector<std::string> argv;
        argv.push_back("binaryname");
        argv.push_back("--config");
        argv.push_back("config.json");
        std::map<std::string, std::string> env_map;

        parser.setConfig("config.json",
                         "{ comment : [ \"can\", \"be\", \"array\", true ], port : 5,"
                         "  comment : { nestedcomment : \"really descriptive\" },"
                         "  host : \"localhost\" }");

        moe::Value value;
        ASSERT_OK(parser.run(testOpts, argv, env_map, &environment));
        ASSERT_OK(environment.get(moe::Key("port"), &value));
        int port;
        ASSERT_OK(value.get(&port));
        ASSERT_EQUALS(port, 5);
        ASSERT_OK(environment.get(moe::Key("host"), &value));
        std::string host;
        ASSERT_OK(value.get(&host));
        ASSERT_EQUALS(host, "localhost");
    }

    TEST(ChainingInterface, GoodReference) {
        OptionsParserTester parser;
        moe::Environment environment;

        moe::OptionSection testOpts;
        // This test is to make sure our reference stays good even after we add more options.  This
        // would not be true if we were using a std::vector in our option section which may need to
        // be moved and resized.
        moe::OptionDescription& optionRef = testOpts.addOptionChaining("ref", "ref", moe::String,
                                                                       "Save this Reference");
        int i;
        for (i = 0; i < 100; i++) {
            ::mongo::StringBuilder sb;
            sb << "filler" << i;
            testOpts.addOptionChaining(sb.str(), sb.str(), moe::String, "Filler Option");
        }
        moe::Value defaultVal(std::string("default"));
        moe::Value implicitVal(std::string("implicit"));
        optionRef.hidden().setDefault(defaultVal);
        optionRef.setImplicit(implicitVal);

        std::vector<moe::OptionDescription> options_vector;
        ASSERT_OK(testOpts.getAllOptions(&options_vector));

        bool foundRef = false;
        for(std::vector<moe::OptionDescription>::const_iterator iterator = options_vector.begin();
            iterator != options_vector.end(); iterator++) {

            if (iterator->_dottedName == "ref") {
                ASSERT_EQUALS(iterator->_singleName, "ref");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "Save this Reference");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.equal(defaultVal));
                ASSERT_TRUE(iterator->_implicit.equal(implicitVal));
                ASSERT_EQUALS(iterator->_isComposing, false);
                foundRef = true;
            }
        }
        if (!foundRef) {
            FAIL("Could not find \"ref\" options that we registered");
        }
    }

    TEST(ChainingInterface, Basic) {
        OptionsParserTester parser;
        moe::Environment environment;

        moe::OptionSection testOpts;
        testOpts.addOptionChaining("basic",
                                   "basic",
                                   moe::String,
                                   "Default Option");

        std::vector<moe::OptionDescription> options_vector;
        ASSERT_OK(testOpts.getAllOptions(&options_vector));

        for(std::vector<moe::OptionDescription>::const_iterator iterator = options_vector.begin();
            iterator != options_vector.end(); iterator++) {

            if (iterator->_dottedName == "basic") {
                ASSERT_EQUALS(iterator->_singleName, "basic");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "Default Option");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
            }
            else {
                ::mongo::StringBuilder sb;
                sb << "Found extra option: " << iterator->_dottedName <<
                      " which we did not register";
                FAIL(sb.str());
            }
        }
    }

    TEST(ChainingInterface, Hidden) {
        OptionsParserTester parser;
        moe::Environment environment;

        moe::OptionSection testOpts;
        testOpts.addOptionChaining("hidden",
                                   "hidden",
                                   moe::String,
                                   "Hidden Option").hidden();

        std::vector<moe::OptionDescription> options_vector;
        ASSERT_OK(testOpts.getAllOptions(&options_vector));

        for(std::vector<moe::OptionDescription>::const_iterator iterator = options_vector.begin();
            iterator != options_vector.end(); iterator++) {

            if (iterator->_dottedName == "hidden") {
                ASSERT_EQUALS(iterator->_singleName, "hidden");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "Hidden Option");
                ASSERT_EQUALS(iterator->_isVisible, false);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
            }
            else {
                ::mongo::StringBuilder sb;
                sb << "Found extra option: " << iterator->_dottedName <<
                      " which we did not register";
                FAIL(sb.str());
            }
        }
    }

    TEST(ChainingInterface, DefaultValue) {
        OptionsParserTester parser;
        moe::Environment environment;

        moe::Value defaultVal(std::string("default"));

        moe::OptionSection testOpts;
        testOpts.addOptionChaining("default",
                                   "default",
                                   moe::String,
                                   "Option With Default Value").setDefault(defaultVal);

        std::vector<moe::OptionDescription> options_vector;
        ASSERT_OK(testOpts.getAllOptions(&options_vector));

        for(std::vector<moe::OptionDescription>::const_iterator iterator = options_vector.begin();
            iterator != options_vector.end(); iterator++) {

            if (iterator->_dottedName == "default") {
                ASSERT_EQUALS(iterator->_singleName, "default");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "Option With Default Value");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.equal(defaultVal));
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, false);
            }
            else {
                ::mongo::StringBuilder sb;
                sb << "Found extra option: " << iterator->_dottedName <<
                      " which we did not register";
                FAIL(sb.str());
            }
        }
    }

    TEST(ChainingInterface, ImplicitValue) {
        OptionsParserTester parser;
        moe::Environment environment;

        moe::Value implicitVal(std::string("implicit"));

        moe::OptionSection testOpts;
        testOpts.addOptionChaining("implicit",
                                   "implicit",
                                   moe::String,
                                   "Option With Implicit Value").setImplicit(implicitVal);

        std::vector<moe::OptionDescription> options_vector;
        ASSERT_OK(testOpts.getAllOptions(&options_vector));

        for(std::vector<moe::OptionDescription>::const_iterator iterator = options_vector.begin();
            iterator != options_vector.end(); iterator++) {

            if (iterator->_dottedName == "implicit") {
                ASSERT_EQUALS(iterator->_singleName, "implicit");
                ASSERT_EQUALS(iterator->_type, moe::String);
                ASSERT_EQUALS(iterator->_description, "Option With Implicit Value");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.equal(implicitVal));
                ASSERT_EQUALS(iterator->_isComposing, false);
            }
            else {
                ::mongo::StringBuilder sb;
                sb << "Found extra option: " << iterator->_dottedName <<
                      " which we did not register";
                FAIL(sb.str());
            }
        }
    }

    TEST(ChainingInterface, Composing) {
        OptionsParserTester parser;
        moe::Environment environment;

        moe::OptionSection testOpts;
        testOpts.addOptionChaining("setParameter",
                                   "setParameter",
                                   moe::StringVector,
                                   "Multiple Values").composing();

        std::vector<moe::OptionDescription> options_vector;
        ASSERT_OK(testOpts.getAllOptions(&options_vector));

        for(std::vector<moe::OptionDescription>::const_iterator iterator = options_vector.begin();
            iterator != options_vector.end(); iterator++) {

            if (iterator->_dottedName == "setParameter") {
                ASSERT_EQUALS(iterator->_singleName, "setParameter");
                ASSERT_EQUALS(iterator->_type, moe::StringVector);
                ASSERT_EQUALS(iterator->_description, "Multiple Values");
                ASSERT_EQUALS(iterator->_isVisible, true);
                ASSERT_TRUE(iterator->_default.isEmpty());
                ASSERT_TRUE(iterator->_implicit.isEmpty());
                ASSERT_EQUALS(iterator->_isComposing, true);
            }
            else {
                ::mongo::StringBuilder sb;
                sb << "Found extra option: " << iterator->_dottedName <<
                      " which we did not register";
                FAIL(sb.str());
            }
        }
    }

    TEST(OptionSources, SourceCommandLine) {
        OptionsParserTester parser;
        moe::Environment environment;
        moe::Value value;
        std::vector<std::string> argv;
        std::map<std::string, std::string> env_map;
        std::string parameter;

        moe::OptionSection testOpts;
        testOpts.addOptionChaining("config", "config", moe::String, "Config file to parse");
        testOpts.addOptionChaining("parameter", "parameter", moe::String, "Parameter")
                                  .setSources(moe::SourceCommandLine);

        argv.clear();
        argv.push_back("binaryname");
        argv.push_back("--parameter");
        argv.push_back("allowed");

        ASSERT_OK(parser.run(testOpts, argv, env_map, &environment));
        ASSERT_OK(environment.get(moe::Key("parameter"), &value));
        ASSERT_OK(value.get(&parameter));
        ASSERT_EQUALS(parameter, "allowed");

        argv.clear();
        argv.push_back("binaryname");
        argv.push_back("--config");
        argv.push_back("config.json");

        parser.setConfig("config.json", "{ parameter : \"disallowed\" }");

        ASSERT_NOT_OK(parser.run(testOpts, argv, env_map, &environment));

        argv.clear();
        argv.push_back("binaryname");
        argv.push_back("--config");
        argv.push_back("config.ini");

        parser.setConfig("config.ini", "parameter=disallowed");

        ASSERT_NOT_OK(parser.run(testOpts, argv, env_map, &environment));
    }

    TEST(OptionSources, SourceINIConfig) {
        OptionsParserTester parser;
        moe::Environment environment;
        moe::Value value;
        std::vector<std::string> argv;
        std::map<std::string, std::string> env_map;
        std::string parameter;

        moe::OptionSection testOpts;
        testOpts.addOptionChaining("config", "config", moe::String, "Config file to parse");
        testOpts.addOptionChaining("parameter", "parameter", moe::String, "Parameter")
                                  .setSources(moe::SourceINIConfig);

        argv.clear();
        argv.push_back("binaryname");
        argv.push_back("--parameter");
        argv.push_back("disallowed");

        ASSERT_NOT_OK(parser.run(testOpts, argv, env_map, &environment));

        argv.clear();
        argv.push_back("binaryname");
        argv.push_back("--config");
        argv.push_back("config.json");

        parser.setConfig("config.json", "{ parameter : \"disallowed\" }");

        ASSERT_NOT_OK(parser.run(testOpts, argv, env_map, &environment));

        argv.clear();
        argv.push_back("binaryname");
        argv.push_back("--config");
        argv.push_back("config.ini");

        parser.setConfig("config.ini", "parameter=allowed");

        ASSERT_OK(parser.run(testOpts, argv, env_map, &environment));
        ASSERT_OK(environment.get(moe::Key("parameter"), &value));
        ASSERT_OK(value.get(&parameter));
        ASSERT_EQUALS(parameter, "allowed");

    }

    TEST(OptionSources, SourceJSONConfig) {
        OptionsParserTester parser;
        moe::Environment environment;
        moe::Value value;
        std::vector<std::string> argv;
        std::map<std::string, std::string> env_map;
        std::string parameter;

        moe::OptionSection testOpts;
        testOpts.addOptionChaining("config", "config", moe::String, "Config file to parse");
        testOpts.addOptionChaining("parameter", "parameter", moe::String, "Parameter")
                                  .setSources(moe::SourceJSONConfig);

        argv.clear();
        argv.push_back("binaryname");
        argv.push_back("--parameter");
        argv.push_back("disallowed");

        ASSERT_NOT_OK(parser.run(testOpts, argv, env_map, &environment));

        argv.clear();
        argv.push_back("binaryname");
        argv.push_back("--config");
        argv.push_back("config.json");

        parser.setConfig("config.json", "{ parameter : \"allowed\" }");

        ASSERT_OK(parser.run(testOpts, argv, env_map, &environment));
        ASSERT_OK(environment.get(moe::Key("parameter"), &value));
        ASSERT_OK(value.get(&parameter));
        ASSERT_EQUALS(parameter, "allowed");

        argv.clear();
        argv.push_back("binaryname");
        argv.push_back("--config");
        argv.push_back("config.ini");

        parser.setConfig("config.ini", "parameter=disallowed");

        ASSERT_NOT_OK(parser.run(testOpts, argv, env_map, &environment));
    }

    TEST(OptionSources, SourceAllConfig) {
        OptionsParserTester parser;
        moe::Environment environment;
        moe::Value value;
        std::vector<std::string> argv;
        std::map<std::string, std::string> env_map;
        std::string parameter;

        moe::OptionSection testOpts;
        testOpts.addOptionChaining("config", "config", moe::String, "Config file to parse");
        testOpts.addOptionChaining("parameter", "parameter", moe::String, "Parameter")
                                  .setSources(moe::SourceAllConfig);

        argv.clear();
        argv.push_back("binaryname");
        argv.push_back("--parameter");
        argv.push_back("disallowed");

        ASSERT_NOT_OK(parser.run(testOpts, argv, env_map, &environment));

        argv.clear();
        argv.push_back("binaryname");
        argv.push_back("--config");
        argv.push_back("config.json");

        parser.setConfig("config.json", "{ parameter : \"allowed\" }");

        ASSERT_OK(parser.run(testOpts, argv, env_map, &environment));
        ASSERT_OK(environment.get(moe::Key("parameter"), &value));
        ASSERT_OK(value.get(&parameter));
        ASSERT_EQUALS(parameter, "allowed");

        argv.clear();
        argv.push_back("binaryname");
        argv.push_back("--config");
        argv.push_back("config.ini");

        parser.setConfig("config.ini", "parameter=allowed");

        ASSERT_OK(parser.run(testOpts, argv, env_map, &environment));
        ASSERT_OK(environment.get(moe::Key("parameter"), &value));
        ASSERT_OK(value.get(&parameter));
        ASSERT_EQUALS(parameter, "allowed");
    }

    TEST(OptionSources, SourceAllLegacy) {
        OptionsParserTester parser;
        moe::Environment environment;
        moe::Value value;
        std::vector<std::string> argv;
        std::map<std::string, std::string> env_map;
        std::string parameter;

        moe::OptionSection testOpts;
        testOpts.addOptionChaining("config", "config", moe::String, "Config file to parse");
        testOpts.addOptionChaining("parameter", "parameter", moe::String, "Parameter")
                                  .setSources(moe::SourceAllLegacy);

        argv.clear();
        argv.push_back("binaryname");
        argv.push_back("--parameter");
        argv.push_back("allowed");

        ASSERT_OK(parser.run(testOpts, argv, env_map, &environment));
        ASSERT_OK(environment.get(moe::Key("parameter"), &value));
        ASSERT_OK(value.get(&parameter));
        ASSERT_EQUALS(parameter, "allowed");

        argv.clear();
        argv.push_back("binaryname");
        argv.push_back("--config");
        argv.push_back("config.json");

        parser.setConfig("config.json", "{ parameter : \"disallowed\" }");

        ASSERT_NOT_OK(parser.run(testOpts, argv, env_map, &environment));

        argv.clear();
        argv.push_back("binaryname");
        argv.push_back("--config");
        argv.push_back("config.ini");

        parser.setConfig("config.ini", "parameter=allowed");

        ASSERT_OK(parser.run(testOpts, argv, env_map, &environment));
        ASSERT_OK(environment.get(moe::Key("parameter"), &value));
        ASSERT_OK(value.get(&parameter));
        ASSERT_EQUALS(parameter, "allowed");
    }

    TEST(OptionSources, SourceAll) {
        OptionsParserTester parser;
        moe::Environment environment;
        moe::Value value;
        std::vector<std::string> argv;
        std::map<std::string, std::string> env_map;
        std::string parameter;

        moe::OptionSection testOpts;
        testOpts.addOptionChaining("config", "config", moe::String, "Config file to parse");
        testOpts.addOptionChaining("parameter", "parameter", moe::String, "Parameter")
                                  .setSources(moe::SourceAll);

        argv.clear();
        argv.push_back("binaryname");
        argv.push_back("--parameter");
        argv.push_back("allowed");

        ASSERT_OK(parser.run(testOpts, argv, env_map, &environment));
        ASSERT_OK(environment.get(moe::Key("parameter"), &value));
        ASSERT_OK(value.get(&parameter));
        ASSERT_EQUALS(parameter, "allowed");

        argv.clear();
        argv.push_back("binaryname");
        argv.push_back("--config");
        argv.push_back("config.json");

        parser.setConfig("config.json", "{ parameter : \"allowed\" }");

        ASSERT_OK(parser.run(testOpts, argv, env_map, &environment));
        ASSERT_OK(environment.get(moe::Key("parameter"), &value));
        ASSERT_OK(value.get(&parameter));
        ASSERT_EQUALS(parameter, "allowed");

        argv.clear();
        argv.push_back("binaryname");
        argv.push_back("--config");
        argv.push_back("config.ini");

        parser.setConfig("config.ini", "parameter=allowed");

        ASSERT_OK(parser.run(testOpts, argv, env_map, &environment));
        ASSERT_OK(environment.get(moe::Key("parameter"), &value));
        ASSERT_OK(value.get(&parameter));
        ASSERT_EQUALS(parameter, "allowed");
    }
} // unnamed namespace
