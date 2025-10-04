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

#include "mongo/util/options_parser/options_parser.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/options_parser/environment.h"
#include "mongo/util/options_parser/option_description.h"
#include "mongo/util/options_parser/option_section.h"
#include "mongo/util/options_parser/value.h"

#include <cstddef>
#include <exception>
#include <fstream>  // IWYU pragma: keep
#include <map>
#include <memory>
#include <utility>

#include <boost/filesystem/path.hpp>

namespace {

using mongo::ErrorCodes;
using mongo::Status;

namespace moe = mongo::optionenvironment;
constexpr auto OptionParserTest = moe::OptionSection::OptionParserUsageType::OptionParserTest;

#define TEST_CONFIG_PATH(x) "src/mongo/util/options_parser/test_config_files/" x

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

TEST(Registration, EmptySingleName) {
    moe::OptionSection testOpts;
    try {
        testOpts.addOptionChaining("dup", "", moe::Switch, "dup", {}, {}, OptionParserTest);
        testOpts.addOptionChaining("new", "", moe::Switch, "dup", {}, {}, OptionParserTest);
    } catch (::mongo::DBException& e) {
        ::mongo::StringBuilder sb;
        sb << "Was not able to register two options with empty single name: " << e.what();
        FAIL(sb.str());
    }

    // This should fail now, because we didn't specify that these options were not valid in the
    // INI config or on the command line
    std::vector<std::string> argv;
    moe::OptionsParser parser;
    moe::Environment environment;
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));

    moe::OptionSection testOptsValid;
    try {
        testOptsValid.addOptionChaining("dup", "", moe::Switch, "dup", {}, {}, OptionParserTest)
            .setSources(moe::SourceYAMLConfig);
        testOptsValid.addOptionChaining("new", "", moe::Switch, "dup", {}, {}, OptionParserTest)
            .setSources(moe::SourceYAMLConfig);
    } catch (::mongo::DBException& e) {
        ::mongo::StringBuilder sb;
        sb << "Was not able to register two options with empty single name" << e.what();
        FAIL(sb.str());
    }

    // This should pass now, because we specified that these options were not valid in the INI
    // config or on the command line
    ASSERT_OK(parser.run(testOptsValid, argv, &environment));
}

TEST(Registration, DuplicateSingleName) {
    moe::OptionSection testOpts;
    try {
        testOpts.addOptionChaining("dup", "dup", moe::Switch, "dup", {}, {}, OptionParserTest);
        testOpts.addOptionChaining("new", "dup", moe::Switch, "dup", {}, {}, OptionParserTest);
        FAIL("Was able to register duplicate single name");
    } catch (::mongo::DBException&) {
    }
}

TEST(Registration, DuplicateSeingleNameAcrossSections) {
    moe::OptionSection group1;
    group1.addOptionChaining("one", "", moe::Switch, "Uno", {}, {}, OptionParserTest);

    moe::OptionSection group2;
    group2.addOptionChaining("one", "", moe::Switch, "Dos", {}, {}, OptionParserTest);

    moe::OptionSection root;
    ASSERT_OK(root.addSection(group1));
    ASSERT_NOT_OK(root.addSection(group2));

    ASSERT_THROWS(root.addOptionChaining("one", "", moe::Switch, "Tres", {}, {}, OptionParserTest),
                  mongo::AssertionException);
    root.addOptionChaining("two", "", moe::Switch, "Quatro", {}, {}, OptionParserTest);

    moe::OptionSection group3;
    group3.addOptionChaining("two", "", moe::Switch, "Cinco", {}, {}, OptionParserTest);
    ASSERT_NOT_OK(root.addSection(group3));
}

TEST(Registration, DuplicateDottedName) {
    moe::OptionSection testOpts;
    try {
        testOpts.addOptionChaining("dup", "dup", moe::Switch, "dup", {}, {}, OptionParserTest);
        testOpts.addOptionChaining("dup", "new", moe::Switch, "dup", {}, {}, OptionParserTest);
        FAIL("Was able to register duplicate single name");
    } catch (::mongo::DBException&) {
    }
}

TEST(Registration, DuplicatePositional) {
    moe::OptionSection testOpts;
    try {
        testOpts
            .addOptionChaining(
                "positional", "positional", moe::Int, "Positional", {}, {}, OptionParserTest)
            .positional(1, 1);
        testOpts
            .addOptionChaining(
                "positional", "positional", moe::Int, "Positional", {}, {}, OptionParserTest)
            .positional(1, 1);
        FAIL("Was able to register duplicate positional option");
    } catch (::mongo::DBException&) {
    }
}

TEST(Registration, BadRangesPositional) {
    moe::OptionSection testOpts;
    try {
        testOpts
            .addOptionChaining(
                "positional1", "positional1", moe::String, "Positional", {}, {}, OptionParserTest)
            .positional(-1, 1);
        FAIL("Was able to register positional with negative start for range");
    } catch (::mongo::DBException&) {
    }
    try {
        testOpts
            .addOptionChaining(
                "positional1", "positional1", moe::String, "Positional", {}, {}, OptionParserTest)
            .positional(2, 1);
        FAIL("Was able to register positional with start of range larger than end");
    } catch (::mongo::DBException&) {
    }
    try {
        testOpts
            .addOptionChaining(
                "positional1", "positional1", moe::String, "Positional", {}, {}, OptionParserTest)
            .positional(1, -2);
        FAIL("Was able to register positional with bad end of range");
    } catch (::mongo::DBException&) {
    }
    try {
        testOpts
            .addOptionChaining(
                "positional1", "positional1", moe::String, "Positional", {}, {}, OptionParserTest)
            .positional(0, 1);
        FAIL("Was able to register positional with bad start of range");
    } catch (::mongo::DBException&) {
    }
    try {
        testOpts
            .addOptionChaining(
                "positional1", "positional1", moe::String, "Positional", {}, {}, OptionParserTest)
            .positional(1, 2);
        FAIL("Was able to register multi valued positional with non StringVector type");
    } catch (::mongo::DBException&) {
    }
}

TEST(Registration, DefaultValueWrongType) {
    moe::OptionSection testOpts;
    try {
        testOpts.addOptionChaining("port", "port", moe::Int, "Port", {}, {}, OptionParserTest)
            .setDefault(moe::Value("String"));
        FAIL("Was able to register default value with wrong type");
    } catch (::mongo::DBException&) {
    }
}

TEST(Registration, ImplicitValueWrongType) {
    moe::OptionSection testOpts;
    try {
        testOpts.addOptionChaining("port", "port", moe::Int, "Port", {}, {}, OptionParserTest)
            .setImplicit(moe::Value("String"));
        FAIL("Was able to register implicit value with wrong type");
    } catch (::mongo::DBException&) {
    }
}

TEST(Registration, ComposableNotVectorOrMap) {
    moe::OptionSection testOpts;
    try {
        testOpts
            .addOptionChaining("setParameter",
                               "setParameter",
                               moe::String,
                               "Multiple Values",
                               {},
                               {},
                               OptionParserTest)
            .composing();
        FAIL("Was able to register composable option with wrong type");
    } catch (::mongo::DBException&) {
    }
}

TEST(Registration, ComposableWithImplicit) {
    moe::OptionSection testOpts;
    try {
        std::vector<std::string> implicitVal;
        implicitVal.push_back("implicit");
        testOpts
            .addOptionChaining("setParameter",
                               "setParameter",
                               moe::StringVector,
                               "Multiple Values",
                               {},
                               {},
                               OptionParserTest)
            .setImplicit(moe::Value(implicitVal))
            .composing();
        FAIL("Was able to register composable option with implicit value");
    } catch (::mongo::DBException&) {
    }

    try {
        std::vector<std::string> implicitVal;
        implicitVal.push_back("implicit");
        testOpts
            .addOptionChaining("setParameter",
                               "setParameter",
                               moe::StringVector,
                               "Multiple Values",
                               {},
                               {},
                               OptionParserTest)
            .composing()
            .setImplicit(moe::Value(implicitVal));
        FAIL("Was able to set implicit value on composable option");
    } catch (::mongo::DBException&) {
    }
}

TEST(Registration, ComposableWithDefault) {
    moe::OptionSection testOpts;
    try {
        std::vector<std::string> defaultVal;
        defaultVal.push_back("default");
        testOpts
            .addOptionChaining("setParameter",
                               "setParameter",
                               moe::StringVector,
                               "Multiple Values",
                               {},
                               {},
                               OptionParserTest)
            .setDefault(moe::Value(defaultVal))
            .composing();
        FAIL("Was able to register composable option with default value");
    } catch (::mongo::DBException&) {
    }

    try {
        std::vector<std::string> defaultVal;
        defaultVal.push_back("default");
        testOpts
            .addOptionChaining("setParameter",
                               "setParameter",
                               moe::StringVector,
                               "Multiple Values",
                               {},
                               {},
                               OptionParserTest)
            .composing()
            .setDefault(moe::Value(defaultVal));
        FAIL("Was able to set default value on composable option");
    } catch (::mongo::DBException&) {
    }
}

TEST(Registration, NestedSubSections) {
    moe::OptionSection root;
    moe::OptionSection childSection;
    moe::OptionSection grandchildSection;

    ASSERT_OK(childSection.addSection(grandchildSection));
    ASSERT_NOT_OK(root.addSection(childSection));
}

TEST(Registration, MergeSubSections) {
    moe::OptionSection root;
    ASSERT_EQ(root.countSubSections(), 0UL);

    {
        moe::OptionSection opts("Options");
        opts.addOptionChaining(
            "option1", "option1", moe::String, "A string option", {}, {}, OptionParserTest);
        ASSERT_OK(root.addSection(opts));
        ASSERT_EQ(root.countSubSections(), 1UL);
    }

    {
        moe::OptionSection moreOpts("Options");
        moreOpts.addOptionChaining(
            "option2", "option2", moe::Int, "An integer option", {}, {}, OptionParserTest);
        ASSERT_OK(root.addSection(moreOpts));
        ASSERT_EQ(root.countSubSections(), 1UL);
    }

    std::vector<moe::OptionDescription> options;
    ASSERT_OK(root.getAllOptions(&options));
    ASSERT_EQ(options.size(), 2UL);
    ASSERT_EQ(options[0]._dottedName, "option1");
    ASSERT_EQ(options[1]._dottedName, "option2");
}

TEST(Parsing, Good) {
    moe::OptionsParser parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "help", "help", moe::Switch, "Display help", {}, {}, OptionParserTest);
    testOpts.addOptionChaining("port", "port", moe::Int, "Port", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--port");
    argv.push_back("5");
    argv.push_back("--help");

    moe::Value value;
    ASSERT_OK(parser.run(testOpts, argv, &environment));
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

    subSection.addOptionChaining("port", "port", moe::Int, "Port", {}, {}, OptionParserTest);
    ASSERT_OK(testOpts.addSection(subSection));

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--port");
    argv.push_back("5");

    moe::Value value;
    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("port"), &value));
    int port;
    ASSERT_OK(value.get(&port));
    ASSERT_EQUALS(port, 5);
}

TEST(Parsing, StringVector) {
    moe::OptionsParser parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "multival", "multival", moe::StringVector, "Multiple Values", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--multival");
    argv.push_back("val1");
    argv.push_back("--multival");
    argv.push_back("val2");

    moe::Value value;
    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("multival"), &value));
    std::vector<std::string> multival;
    std::vector<std::string>::iterator multivalit;
    ASSERT_OK(value.get(&multival));
    multivalit = multival.begin();
    ASSERT_EQUALS(*multivalit, "val1");
    multivalit++;
    ASSERT_EQUALS(*multivalit, "val2");
}

TEST(Parsing, StringMap) {
    moe::OptionsParser parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "multival", "multival", moe::StringMap, "Multiple Values", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--multival");
    argv.push_back("key1=value1");
    argv.push_back("--multival");
    argv.push_back("key2=value2");
    argv.push_back("--multival");
    argv.push_back("key3=");

    moe::Value value;
    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("multival"), &value));
    std::map<std::string, std::string> multival;
    std::map<std::string, std::string>::iterator multivalit;
    ASSERT_OK(value.get(&multival));
    multivalit = multival.begin();
    ASSERT_EQUALS(multivalit->first, "key1");
    ASSERT_EQUALS(multivalit->second, "value1");
    multivalit++;
    ASSERT_EQUALS(multivalit->first, "key2");
    ASSERT_EQUALS(multivalit->second, "value2");
    multivalit++;
    ASSERT_EQUALS(multivalit->first, "key3");
    ASSERT_EQUALS(multivalit->second, "");
}

TEST(Parsing, StringMapDuplicateKey) {
    moe::OptionsParser parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "multival", "multival", moe::StringMap, "Multiple Values", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--multival");
    argv.push_back("key1=value1");
    argv.push_back("--multival");
    argv.push_back("key1=value2");

    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
}

TEST(Parsing, Positional) {
    moe::OptionsParser parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts
        .addOptionChaining(
            "positional", "positional", moe::String, "Positional", {}, {}, OptionParserTest)
        .positional(1, 1);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("positional");

    moe::Value value;
    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("positional"), &value));
    std::string positional;
    ASSERT_OK(value.get(&positional));
    ASSERT_EQUALS(positional, "positional");
}

TEST(Parsing, PositionalTooMany) {
    moe::OptionsParser parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts
        .addOptionChaining(
            "positional", "positional", moe::String, "Positional", {}, {}, OptionParserTest)
        .positional(1, 1);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("positional");
    argv.push_back("extrapositional");

    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
}

TEST(Parsing, PositionalAndFlag) {
    moe::OptionsParser parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts
        .addOptionChaining(
            "positional", "positional", moe::String, "Positional", {}, {}, OptionParserTest)
        .positional(1, 1);
    testOpts.addOptionChaining("port", "port", moe::Int, "Port", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("positional");
    argv.push_back("--port");
    argv.push_back("5");

    moe::Value value;
    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("positional"), &value));
    std::string positional;
    ASSERT_OK(value.get(&positional));
    ASSERT_EQUALS(positional, "positional");
    ASSERT_OK(environment.get(moe::Key("port"), &value));
    int port;
    ASSERT_OK(value.get(&port));
    ASSERT_EQUALS(port, 5);
}

TEST(Parsing, PositionalMultiple) {
    moe::OptionsParser parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts
        .addOptionChaining(
            "positional", "positional", moe::StringVector, "Positional", {}, {}, OptionParserTest)
        .positional(1, 2);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("positional1");
    argv.push_back("positional2");

    ASSERT_OK(parser.run(testOpts, argv, &environment));
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
    testOpts
        .addOptionChaining(
            "positional", "positional", moe::StringVector, "Positional", {}, {}, OptionParserTest)
        .positional(1, 2);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("positional1");
    argv.push_back("positional2");
    argv.push_back("positional2");

    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
}

TEST(Parsing, PositionalMultipleUnlimited) {
    moe::OptionsParser parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts
        .addOptionChaining(
            "positional", "positional", moe::StringVector, "Positional", {}, {}, OptionParserTest)
        .positional(1, -1);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("positional1");
    argv.push_back("positional2");
    argv.push_back("positional3");
    argv.push_back("positional4");
    argv.push_back("positional5");

    ASSERT_OK(parser.run(testOpts, argv, &environment));
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
    testOpts
        .addOptionChaining(
            "positional", "positional", moe::StringVector, "Positional", {}, {}, OptionParserTest)
        .positional(1, 2);
    testOpts.addOptionChaining("port", "port", moe::Int, "Port", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("positional1");
    argv.push_back("--port");
    argv.push_back("5");
    argv.push_back("positional2");

    ASSERT_OK(parser.run(testOpts, argv, &environment));
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
    testOpts.addOptionChaining(
        "help", "help", moe::Switch, "Display help", {}, {}, OptionParserTest);
    testOpts.addOptionChaining("port", "port", moe::Int, "Port", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--port");

    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
}

TEST(Parsing, BadArg) {
    moe::OptionsParser parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "help", "help", moe::Switch, "Display help", {}, {}, OptionParserTest);
    testOpts.addOptionChaining("port", "port", moe::Int, "Port", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--port");
    argv.push_back("string");

    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
}

TEST(Parsing, ExtraArg) {
    moe::OptionsParser parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "help", "help", moe::Switch, "Display help", {}, {}, OptionParserTest);
    testOpts.addOptionChaining("port", "port", moe::Int, "Port", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--help");
    argv.push_back("string");

    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
}

TEST(Parsing, DefaultValue) {
    moe::OptionsParser parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "help", "help", moe::Switch, "Display help", {}, {}, OptionParserTest);
    testOpts.addOptionChaining("port", "port", moe::Int, "Port", {}, {}, OptionParserTest)
        .setDefault(moe::Value(5));

    std::vector<std::string> argv;
    argv.push_back("binaryname");

    ASSERT_OK(parser.run(testOpts, argv, &environment));
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
    testOpts.addOptionChaining(
        "help", "help", moe::Switch, "Display help", {}, {}, OptionParserTest);
    testOpts.addOptionChaining("port", "port", moe::Int, "Port", {}, {}, OptionParserTest)
        .setDefault(moe::Value(5));

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--port");
    argv.push_back("6");

    ASSERT_OK(parser.run(testOpts, argv, &environment));
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
    testOpts.addOptionChaining(
        "help", "help", moe::Switch, "Display help", {}, {}, OptionParserTest);
    testOpts.addOptionChaining("val1", "val1", moe::Int, "Val1", {}, {}, OptionParserTest)
        .setDefault(moe::Value(5));
    testOpts.addOptionChaining("val2", "val2", moe::Int, "Val2", {}, {}, OptionParserTest)
        .setDefault(moe::Value(5));

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--val1");
    argv.push_back("6");

    ASSERT_OK(parser.run(testOpts, argv, &environment));

    mongo::BSONObj expected = BSON("val1" << 6);
    ASSERT_BSONOBJ_EQ(expected, environment.toBSON());
}

TEST(Parsing, ImplicitValue) {
    moe::OptionsParser parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "help", "help", moe::Switch, "Display help", {}, {}, OptionParserTest);
    testOpts.addOptionChaining("port", "port", moe::Int, "Port", {}, {}, OptionParserTest)
        .setDefault(moe::Value(6))
        .setImplicit(moe::Value(7));

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--port");

    ASSERT_OK(parser.run(testOpts, argv, &environment));
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
    testOpts.addOptionChaining(
        "help", "help", moe::Switch, "Display help", {}, {}, OptionParserTest);
    testOpts.addOptionChaining("port", "port", moe::Int, "Port", {}, {}, OptionParserTest)
        .setDefault(moe::Value(6))
        .setImplicit(moe::Value(7));

    std::vector<std::string> argv;
    argv.push_back("binaryname");

    ASSERT_OK(parser.run(testOpts, argv, &environment));
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
    testOpts.addOptionChaining(
        "help", "help", moe::Switch, "Display help", {}, {}, OptionParserTest);
    testOpts.addOptionChaining("port", "port", moe::Int, "Port", {}, {}, OptionParserTest)
        .setDefault(moe::Value(6))
        .setImplicit(moe::Value(7));

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--port");
    argv.push_back("5");

    ASSERT_OK(parser.run(testOpts, argv, &environment));
    moe::Value value;
    ASSERT_OK(environment.get(moe::Key("port"), &value));
    int port;
    ASSERT_OK(value.get(&port));
    ASSERT_EQUALS(port, 5);
}

TEST(Parsing, ImplicitValueOverrideWithEqualsSign) {
    moe::OptionsParser parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "help", "help", moe::Switch, "Display help", {}, {}, OptionParserTest);
    testOpts.addOptionChaining("port", "port", moe::Int, "Port", {}, {}, OptionParserTest)
        .setDefault(moe::Value(6))
        .setImplicit(moe::Value(7));

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--port=5");

    ASSERT_OK(parser.run(testOpts, argv, &environment));
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
    testOpts.addOptionChaining(
        "help", "help,h", moe::Switch, "Display help", {}, {}, OptionParserTest);
    testOpts.addOptionChaining("port", "port,p", moe::Int, "Port", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("-p");
    argv.push_back("5");
    argv.push_back("-h");

    moe::Value value;
    ASSERT_OK(parser.run(testOpts, argv, &environment));
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
    testOpts.addOptionChaining("opt", "opt,o", moe::Switch, "first opt", {}, {}, OptionParserTest);
    testOpts.addOptionChaining("arg", "arg,a", moe::Switch, "first arg", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("-oa");

    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
}

TEST(Style, NoGuessing) {
    moe::OptionsParser parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "help", "help", moe::Switch, "Display help", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--hel");

    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
}

TEST(Style, LongDisguises) {
    moe::OptionsParser parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "help", "help", moe::Switch, "Display help", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("-help");

    ASSERT_OK(parser.run(testOpts, argv, &environment));
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
    testOpts.addOptionChaining(
        "v",
        "verbose,v",
        moe::Switch,
        "be more verbose (include multiple times for more verbosity e.g. -vvvvv)",
        {},
        {},
        OptionParserTest);

    /* support for -vv -vvvv etc. */
    for (std::string s = "vv"; s.length() <= 12; s.append("v")) {
        testOpts
            .addOptionChaining(s.c_str(),
                               s.c_str(),
                               moe::Switch,
                               "higher verbosity levels (hidden)",
                               {},
                               {},
                               OptionParserTest)
            .hidden();
    }

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("-vvvvvv");

    ASSERT_OK(parser.run(testOpts, argv, &environment));

    moe::Value value;
    for (std::string s = "vv"; s.length() <= 12; s.append("v")) {
        if (s.length() == 6) {
            ASSERT_OK(environment.get(moe::Key(s), &value));
            bool verbose;
            ASSERT_OK(value.get(&verbose));
            ASSERT_EQUALS(verbose, true);
        } else {
            ASSERT_NOT_OK(environment.get(moe::Key(s), &value));
        }
    }
}

TEST(INIConfigFile, Basic) {
    OptionsParserTester parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);
    testOpts.addOptionChaining("port", "port", moe::Int, "Port", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("default.conf");

    parser.setConfig("default.conf", "port=5");

    ASSERT_OK(parser.run(testOpts, argv, &environment));
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
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("default.conf");

    parser.setConfig("default.conf", "");

    ASSERT_OK(parser.run(testOpts, argv, &environment));
}

TEST(INIConfigFile, Override) {
    OptionsParserTester parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);
    testOpts.addOptionChaining("port", "port", moe::Int, "Port", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("default.conf");
    argv.push_back("--port");
    argv.push_back("6");

    parser.setConfig("default.conf", "port=5");

    ASSERT_OK(parser.run(testOpts, argv, &environment));
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
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);
    testOpts.addOptionChaining("port", "port", moe::Int, "Port", {}, {}, OptionParserTest);
    testOpts.addOptionChaining("str", "str", moe::String, "String", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("default.conf");

    parser.setConfig("default.conf", "# port=5\nstr=NotCommented");

    ASSERT_OK(parser.run(testOpts, argv, &environment));
    moe::Value value;
    ASSERT_NOT_OK(environment.get(moe::Key("port"), &value));
    ASSERT_OK(environment.get(moe::Key("str"), &value));
    std::string str;
    ASSERT_OK(value.get(&str));
    ASSERT_EQUALS(str, "NotCommented");
}

// Ensure switches in INI config files have the correct semantics.
//
// Switches have the following semantics:
// - Present on the command line -> set to true
// - Present in the config file -> set to value in config file
// - Present in the config file with no value (INI only) -> set to true
// - Not present -> not set to any value
TEST(INIConfigFile, Switches) {
    OptionsParserTester parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);
    testOpts.addOptionChaining(
        "switch1", "switch1", moe::Switch, "switch1", {}, {}, OptionParserTest);
    testOpts.addOptionChaining(
        "switch2", "switch2", moe::Switch, "switch2", {}, {}, OptionParserTest);
    testOpts.addOptionChaining(
        "switch3", "switch3", moe::Switch, "switch3", {}, {}, OptionParserTest);
    testOpts.addOptionChaining(
        "switch4", "switch4", moe::Switch, "switch4", {}, {}, OptionParserTest);
    testOpts.addOptionChaining(
        "switch5", "switch5", moe::Switch, "switch5", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("default.conf");
    argv.push_back("--switch1");

    parser.setConfig("default.conf", "switch2=true\nswitch3=false\nswitch5=");

    ASSERT_OK(parser.run(testOpts, argv, &environment));
    bool switch1;
    ASSERT_OK(environment.get(moe::Key("switch1"), &switch1));
    ASSERT_TRUE(switch1);
    bool switch2;
    ASSERT_OK(environment.get(moe::Key("switch2"), &switch2));
    ASSERT_TRUE(switch2);
    bool switch3;
    ASSERT_OK(environment.get(moe::Key("switch3"), &switch3));
    ASSERT_FALSE(switch3);
    bool switch4;
    ASSERT_NOT_OK(environment.get(moe::Key("switch4"), &switch4));
    bool switch5;
    ASSERT_OK(environment.get(moe::Key("switch5"), &switch5));
    ASSERT_TRUE(switch5);
}

TEST(INIConfigFile, Monkeys) {
    OptionsParserTester parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);
    testOpts.addOptionChaining("this", "this", moe::Switch, "This", {}, {}, OptionParserTest);
    testOpts.addOptionChaining("that", "that", moe::Switch, "That", {}, {}, OptionParserTest);
    testOpts.addOptionChaining(
        "another", "another", moe::String, "Another", {}, {}, OptionParserTest);
    testOpts.addOptionChaining("other", "other", moe::String, "Other", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("default.conf");

    parser.setConfig("default.conf",
                     "\t this = false \n#that = true\n #another = whocares"
                     "\n\n other = monkeys ");

    ASSERT_OK(parser.run(testOpts, argv, &environment));
    moe::Value value;
    ASSERT_OK(environment.get(moe::Key("this"), &value));
    bool thisValue;
    ASSERT_OK(value.get(&thisValue));
    ASSERT_FALSE(thisValue);
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
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);
    testOpts.addOptionChaining("port", "port", moe::Int, "Port", {}, {}, OptionParserTest)
        .setDefault(moe::Value(5));

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("default.conf");

    parser.setConfig("default.conf", "port=6");

    ASSERT_OK(parser.run(testOpts, argv, &environment));
    moe::Value value;
    ASSERT_OK(environment.get(moe::Key("port"), &value));
    int port;
    ASSERT_OK(value.get(&port));
    ASSERT_EQUALS(port, 6);
}

TEST(INIConfigFile, StringVector) {
    OptionsParserTester parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);
    testOpts.addOptionChaining(
        "multival", "multival", moe::StringVector, "Multiple Values", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.ini");

    parser.setConfig("config.ini", "multival = val1\nmultival = val2");

    moe::Value value;
    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("multival"), &value));
    std::vector<std::string> multival;
    std::vector<std::string>::iterator multivalit;
    ASSERT_OK(value.get(&multival));
    multivalit = multival.begin();
    ASSERT_EQUALS(*multivalit, "val1");
    multivalit++;
    ASSERT_EQUALS(*multivalit, "val2");
}

TEST(INIConfigFile, StringMap) {
    OptionsParserTester parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);
    testOpts.addOptionChaining(
        "multival", "multival", moe::StringMap, "Multiple Values", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.ini");

    parser.setConfig("config.ini",
                     "multival = key1=value1\n"
                     "multival = key2=value2\n"
                     "multival = key3=");

    moe::Value value;
    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("multival"), &value));
    std::map<std::string, std::string> multival;
    std::map<std::string, std::string>::iterator multivalit;
    ASSERT_OK(value.get(&multival));
    multivalit = multival.begin();
    ASSERT_EQUALS(multivalit->first, "key1");
    ASSERT_EQUALS(multivalit->second, "value1");
    multivalit++;
    ASSERT_EQUALS(multivalit->first, "key2");
    ASSERT_EQUALS(multivalit->second, "value2");
    multivalit++;
    ASSERT_EQUALS(multivalit->first, "key3");
    ASSERT_EQUALS(multivalit->second, "");
}

TEST(INIConfigFile, StringMapDuplicateKey) {
    OptionsParserTester parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);
    testOpts.addOptionChaining(
        "multival", "multival", moe::StringMap, "Multiple Values", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.ini");

    parser.setConfig("config.ini",
                     "multival = key1=value1\n"
                     "multival = key1=value2");

    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
}

TEST(JSONConfigFile, Basic) {
    OptionsParserTester parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);
    testOpts.addOptionChaining("port", "port", moe::Int, "Port", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.json");

    parser.setConfig("config.json", "{ port : 5 }");

    ASSERT_OK(parser.run(testOpts, argv, &environment));
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
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.json");

    parser.setConfig("config.json", "");

    ASSERT_OK(parser.run(testOpts, argv, &environment));
}

TEST(JSONConfigFile, EmptyObject) {
    OptionsParserTester parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.json");

    parser.setConfig("config.json", "{}");

    ASSERT_OK(parser.run(testOpts, argv, &environment));
}

TEST(JSONConfigFile, Override) {
    OptionsParserTester parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);
    testOpts.addOptionChaining("port", "port", moe::Int, "Port", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.json");
    argv.push_back("--port");
    argv.push_back("6");


    parser.setConfig("config.json", "{ port : 5 }");

    ASSERT_OK(parser.run(testOpts, argv, &environment));
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
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.json");

    parser.setConfig("config.json", "{ port : 5 }");

    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
}

TEST(JSONConfigFile, DuplicateOption) {
    OptionsParserTester parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);
    testOpts.addOptionChaining("port", "port", moe::Int, "Port", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.json");

    parser.setConfig("config.json", "{ port : 5, port : 5 }");

    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
}

TEST(JSONConfigFile, TypeChecking) {
    OptionsParserTester parser;
    moe::Environment environment;
    moe::Value value;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);
    testOpts.addOptionChaining("stringVectorVal",
                               "stringVectorVal",
                               moe::StringVector,
                               "StringVectorVal",
                               {},
                               {},
                               OptionParserTest);
    testOpts.addOptionChaining(
        "boolVal", "boolVal", moe::Bool, "BoolVal", {}, {}, OptionParserTest);
    testOpts.addOptionChaining(
        "doubleVal", "doubleVal", moe::Double, "DoubleVal", {}, {}, OptionParserTest);
    testOpts.addOptionChaining("intVal", "intVal", moe::Int, "IntVal", {}, {}, OptionParserTest);
    testOpts.addOptionChaining(
        "longVal", "longVal", moe::Long, "LongVal", {}, {}, OptionParserTest);
    testOpts.addOptionChaining(
        "stringVal", "stringVal", moe::String, "StringVal", {}, {}, OptionParserTest);
    testOpts.addOptionChaining("unsignedLongLongVal",
                               "unsignedLongLongVal",
                               moe::UnsignedLongLong,
                               "UnsignedLongLongVal",
                               {},
                               {},
                               OptionParserTest);
    testOpts.addOptionChaining(
        "unsignedVal", "unsignedVal", moe::Unsigned, "UnsignedVal", {}, {}, OptionParserTest);
    testOpts.addOptionChaining(
        "switchVal", "switchVal", moe::Switch, "SwitchVal", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.json");

    // Test StringVector type
    std::vector<std::string> stringVectorVal;

    parser.setConfig("config.json", "{ stringVectorVal : \"scalar\" }");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
    environment = moe::Environment();
    parser.setConfig("config.json", "{ stringVectorVal : \"true\" }");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
    environment = moe::Environment();
    parser.setConfig("config.json", "{ stringVectorVal : \"5\" }");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
    environment = moe::Environment();
    parser.setConfig("config.json", "{ stringVectorVal : [ [ \"string\" ], true, 1, 1.0 ] }");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));

    // The YAML parser treats everything as a string, so we just take anything that was
    // specified as a string vector type and treat it as an array of strings, even if the
    // elements are not surrounded by quotes
    environment = moe::Environment();
    parser.setConfig("config.json", "{ stringVectorVal : [ \"string\", bare, true, 1, 1.0 ] }");
    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("stringVectorVal"), &value));
    std::vector<std::string>::iterator stringVectorValIt;
    ASSERT_OK(value.get(&stringVectorVal));
    stringVectorValIt = stringVectorVal.begin();
    ASSERT_EQUALS(*stringVectorValIt, "string");
    stringVectorValIt++;
    ASSERT_EQUALS(*stringVectorValIt, "bare");
    stringVectorValIt++;
    ASSERT_EQUALS(*stringVectorValIt, "true");
    stringVectorValIt++;
    ASSERT_EQUALS(*stringVectorValIt, "1");
    stringVectorValIt++;
    ASSERT_EQUALS(*stringVectorValIt, "1.0");

    // Test Bool type
    bool boolVal;
    environment = moe::Environment();
    parser.setConfig("config.json", "{ boolVal : \"lies\" }");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
    environment = moe::Environment();
    parser.setConfig("config.json", "{ boolVal : truth }");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
    environment = moe::Environment();
    parser.setConfig("config.json", "{ boolVal : 1 }");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));

    // The YAML parser treats everything as a string, so we just take anything that was
    // specified as a bool type and try to convert it to a bool, even if it was quoted
    environment = moe::Environment();
    parser.setConfig("config.json", "{ boolVal : \"true\" }");
    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("boolVal"), &value));
    ASSERT_OK(value.get(&boolVal));
    ASSERT_EQUALS(boolVal, true);
    environment = moe::Environment();
    parser.setConfig("config.json", "{ boolVal : false }");
    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("boolVal"), &value));
    ASSERT_OK(value.get(&boolVal));
    ASSERT_EQUALS(boolVal, false);

    // Test Double type
    double doubleVal;
    environment = moe::Environment();
    parser.setConfig("config.json", "{ doubleVal : \"double the monkeys\" }");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
    environment = moe::Environment();
    parser.setConfig("config.json", "{ doubleVal : true }");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));

    // The YAML parser treats everything as a string, so we just take anything that was
    // specified as a double type and try to convert it to a double, even if it was quoted
    environment = moe::Environment();
    parser.setConfig("config.json", "{ doubleVal : 1.5 }");
    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("doubleVal"), &value));
    ASSERT_OK(value.get(&doubleVal));
    ASSERT_EQUALS(doubleVal, 1.5);
    environment = moe::Environment();
    parser.setConfig("config.json", "{ doubleVal : -1.5 }");
    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("doubleVal"), &value));
    ASSERT_OK(value.get(&doubleVal));
    ASSERT_EQUALS(doubleVal, -1.5);
    environment = moe::Environment();
    parser.setConfig("config.json", "{ doubleVal : \"3.14\" }");
    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("doubleVal"), &value));
    ASSERT_OK(value.get(&doubleVal));
    ASSERT_EQUALS(doubleVal, 3.14);
    environment = moe::Environment();
    parser.setConfig("config.json", "{ doubleVal : \"-3.14\" }");
    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("doubleVal"), &value));
    ASSERT_OK(value.get(&doubleVal));
    ASSERT_EQUALS(doubleVal, -3.14);

    // Test Int type
    int intVal;
    environment = moe::Environment();
    parser.setConfig("config.json", "{ intVal : \"hungry hippos\" }");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
    environment = moe::Environment();
    parser.setConfig("config.json", "{ intVal : 1.5 }");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
    environment = moe::Environment();
    parser.setConfig("config.json", "{ intVal : 18446744073709551617 }");  // 2^64 + 1
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
    environment = moe::Environment();
    parser.setConfig("config.json", "{ intVal : true }");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));

    // The YAML parser treats everything as a string, so we just take anything that was
    // specified as an int type and try to convert it to a int, even if it was quoted
    environment = moe::Environment();
    parser.setConfig("config.json", "{ intVal : \"5\" }");
    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("intVal"), &value));
    ASSERT_OK(value.get(&intVal));
    ASSERT_EQUALS(intVal, 5);

    environment = moe::Environment();
    parser.setConfig("config.json", "{ intVal : \"-5\" }");
    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("intVal"), &value));
    ASSERT_OK(value.get(&intVal));
    ASSERT_EQUALS(intVal, -5);

    // Test Long type
    long longVal;
    environment = moe::Environment();
    parser.setConfig("config.json", "{ longVal : \"in an eating race\" }");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
    environment = moe::Environment();
    parser.setConfig("config.json", "{ longVal : 1.5 }");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
    environment = moe::Environment();
    parser.setConfig("config.json", "{ longVal : 18446744073709551617 }");  // 2^64 + 1
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
    environment = moe::Environment();
    parser.setConfig("config.json", "{ longVal : true }");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));

    // The YAML parser treats everything as a string, so we just take anything that was
    // specified as a long type and try to convert it to a long, even if it was quoted
    environment = moe::Environment();
    parser.setConfig("config.json", "{ longVal : \"5\" }");
    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("longVal"), &value));
    ASSERT_OK(value.get(&longVal));
    ASSERT_EQUALS(longVal, 5);

    environment = moe::Environment();
    parser.setConfig("config.json", "{ longVal : \"-5\" }");
    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("longVal"), &value));
    ASSERT_OK(value.get(&longVal));
    ASSERT_EQUALS(longVal, -5);

    // Test String type
    std::string stringVal;

    // The YAML parser treats everything as a string, so we just take anything that was
    // specified as a string type and treat it as a string, even if the element is not
    // surrounded by quotes
    environment = moe::Environment();
    parser.setConfig("config.json", "{ stringVal : }");
    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("stringVal"), &value));
    ASSERT_OK(value.get(&stringVal));
    ASSERT_EQUALS(stringVal, "");

    environment = moe::Environment();
    parser.setConfig("config.json", "{ stringVal : \"1000\" }");
    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("stringVal"), &value));
    ASSERT_OK(value.get(&stringVal));
    ASSERT_EQUALS(stringVal, "1000");

    environment = moe::Environment();
    parser.setConfig("config.json", "{ stringVal : wat man }");
    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("stringVal"), &value));
    ASSERT_OK(value.get(&stringVal));
    ASSERT_EQUALS(stringVal, "wat man");

    environment = moe::Environment();
    parser.setConfig("config.json", "{ stringVal : true 1 string 1.0 }");
    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("stringVal"), &value));
    ASSERT_OK(value.get(&stringVal));
    ASSERT_EQUALS(stringVal, "true 1 string 1.0");

    // Test UnsignedLongLong type
    unsigned long long unsignedLongLongVal;
    environment = moe::Environment();
    parser.setConfig("config.json", "{ unsignedLongLongVal : \"unsigned hungry hippos\" }");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
    environment = moe::Environment();
    parser.setConfig("config.json", "{ unsignedLongLongVal : 1.5 }");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
    environment = moe::Environment();
    parser.setConfig("config.json", "{ unsignedLongLongVal : 18446744073709551617 }");  // 2^64 + 1
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
    environment = moe::Environment();
    parser.setConfig("config.json", "{ unsignedLongLongVal : true }");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
    environment = moe::Environment();
    parser.setConfig("config.json", "{ unsignedLongLongVal : \"-5\" }");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));

    // The YAML parser treats everything as a string, so we just take anything that was
    // specified as an unsigned long long type and try to convert it to an unsigned long long,
    // even if it was quoted
    environment = moe::Environment();
    parser.setConfig("config.json", "{ unsignedLongLongVal : \"5\" }");
    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("unsignedLongLongVal"), &value));
    ASSERT_OK(value.get(&unsignedLongLongVal));
    ASSERT_EQUALS(unsignedLongLongVal, 5ULL);

    // Test Unsigned type
    unsigned unsignedVal;
    environment = moe::Environment();
    parser.setConfig("config.json", "{ unsignedVal : \"unsigned hungry hippos\" }");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
    environment = moe::Environment();
    parser.setConfig("config.json", "{ unsignedVal : 1.5 }");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
    environment = moe::Environment();
    parser.setConfig("config.json", "{ unsignedVal : 18446744073709551617 }");  // 2^64 + 1
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
    environment = moe::Environment();
    parser.setConfig("config.json", "{ unsignedVal : true }");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
    environment = moe::Environment();
    parser.setConfig("config.json", "{ unsignedVal : \"-5\" }");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));

    // The YAML parser treats everything as a string, so we just take anything that was
    // specified as an unsigned type and try to convert it to an unsigned, even if it was quoted
    environment = moe::Environment();
    parser.setConfig("config.json", "{ unsignedVal : \"5\" }");
    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("unsignedVal"), &value));
    ASSERT_OK(value.get(&unsignedVal));
    ASSERT_EQUALS(unsignedVal, 5U);

    // Test Switch type
    bool switchVal;
    environment = moe::Environment();
    parser.setConfig("config.json", "{ switchVal : \"lies\" }");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
    environment = moe::Environment();
    parser.setConfig("config.json", "{ switchVal : truth }");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
    environment = moe::Environment();
    parser.setConfig("config.json", "{ switchVal : 1 }");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));

    // The YAML parser treats everything as a string, so we just take anything that was
    // specified as a switch type and try to convert it to a bool, even if it was quoted
    environment = moe::Environment();
    parser.setConfig("config.json", "{ switchVal : \"true\" }");
    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("switchVal"), &value));
    ASSERT_OK(value.get(&switchVal));
    ASSERT_EQUALS(switchVal, true);
    environment = moe::Environment();
    parser.setConfig("config.json", "{ switchVal : false }");
    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("switchVal"), &switchVal));
    ASSERT_FALSE(switchVal);
}

TEST(JSONConfigFile, Nested) {
    OptionsParserTester parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);
    testOpts.addOptionChaining("nested.port", "port", moe::Int, "Port", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.json");

    parser.setConfig("config.json", "{ nested : { port : 5 } }");

    ASSERT_OK(parser.run(testOpts, argv, &environment));
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
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);
    testOpts.addOptionChaining("dotted.port", "port", moe::Int, "Port", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.json");

    parser.setConfig("config.json", "{ \"dotted.port\" : 5 }");

    ASSERT_OK(parser.run(testOpts, argv, &environment));
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
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);
    testOpts.addOptionChaining(
        "dottednested.var1", "var1", moe::Int, "Var1", {}, {}, OptionParserTest);
    testOpts.addOptionChaining(
        "dottednested.var2", "var2", moe::Int, "Var2", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.json");

    parser.setConfig("config.json", "{ \"dottednested.var1\" : 5, dottednested : { var2 : 6 } }");

    ASSERT_OK(parser.run(testOpts, argv, &environment));
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
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);
    testOpts.addOptionChaining(
        "multival", "multival", moe::StringVector, "Multiple Values", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.json");

    parser.setConfig("config.json", "{ multival : [ \"val1\", \"val2\" ] }");

    moe::Value value;
    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("multival"), &value));
    std::vector<std::string> multival;
    std::vector<std::string>::iterator multivalit;
    ASSERT_OK(value.get(&multival));
    multivalit = multival.begin();
    ASSERT_EQUALS(*multivalit, "val1");
    multivalit++;
    ASSERT_EQUALS(*multivalit, "val2");
}

TEST(JSONConfigFile, StringMap) {
    OptionsParserTester parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);
    testOpts.addOptionChaining(
        "multival", "multival", moe::StringMap, "Multiple Values", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.json");

    parser.setConfig("config.json",
                     "{ multival : { key1 : \"value1\", key2 : \"value2\", key3 : \"\" } }");

    moe::Value value;
    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("multival"), &value));
    std::map<std::string, std::string> multival;
    std::map<std::string, std::string>::iterator multivalit;
    ASSERT_OK(value.get(&multival));
    multivalit = multival.begin();
    ASSERT_EQUALS(multivalit->first, "key1");
    ASSERT_EQUALS(multivalit->second, "value1");
    multivalit++;
    ASSERT_EQUALS(multivalit->first, "key2");
    ASSERT_EQUALS(multivalit->second, "value2");
    multivalit++;
    ASSERT_EQUALS(multivalit->first, "key3");
    ASSERT_EQUALS(multivalit->second, "");
}

TEST(JSONConfigFile, StringMapDuplicateKey) {
    OptionsParserTester parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);
    testOpts.addOptionChaining(
        "multival", "multival", moe::StringMap, "Multiple Values", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.json");

    parser.setConfig("config.json", "{ multival : { key1 : \"value1\", key1 : \"value2\" } }");

    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
}

TEST(JSONConfigFile, StringVectorNonString) {
    OptionsParserTester parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);
    testOpts.addOptionChaining(
        "multival", "multival", moe::StringVector, "Multiple Values", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.json");

    // NOTE: The yaml config file just reads things as strings, and it's up to us to decide what
    // the type should be later.  This means that we can't tell the difference between when a
    // user provides a non string value or a string value in some cases.
    parser.setConfig("config.json", "{ multival : [ 1, true ] }");

    moe::Value value;
    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("multival"), &value));
    std::vector<std::string> multival;
    std::vector<std::string>::iterator multivalit;
    ASSERT_OK(value.get(&multival));
    multivalit = multival.begin();
    ASSERT_EQUALS(*multivalit, "1");
    multivalit++;
    ASSERT_EQUALS(*multivalit, "true");
}

TEST(JSONConfigFile, DefaultValueOverride) {
    OptionsParserTester parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);
    testOpts.addOptionChaining("port", "port", moe::Int, "Port", {}, {}, OptionParserTest)
        .setDefault(moe::Value(5));

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.json");

    parser.setConfig("config.json", "{ port : 6 }");

    ASSERT_OK(parser.run(testOpts, argv, &environment));
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
    testOpts.addOptionChaining(
        "config", "config", moe::Int, "Config file to parse", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("1");

    parser.setConfig("default.conf", "");

    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
}

TEST(Parsing, MapForScalarMismatch) {
    OptionsParserTester parser;
    moe::Environment environment;
    moe::OptionSection testOpts;

    testOpts.addOptionChaining(
        "config", "config", moe::Int, "Config file to parse", {}, {}, OptionParserTest);
    testOpts.addOptionChaining("str", "str", moe::String, "", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.json");

    parser.setConfig("config.json", R"cfg({ str: { elem: "val" } })cfg");

    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
}

TEST(Parsing, ScalarForMapMismatch) {
    OptionsParserTester parser;
    moe::Environment environment;
    moe::OptionSection testOpts;

    testOpts.addOptionChaining(
        "config", "config", moe::Int, "Config file to parse", {}, {}, OptionParserTest);
    testOpts.addOptionChaining("strmap", "strmap", moe::StringMap, "", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.json");

    parser.setConfig("config.json", R"cfg({ str: "val" })cfg");

    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
}

TEST(Parsing, ListForScalarMismatch) {
    OptionsParserTester parser;
    moe::Environment environment;
    moe::OptionSection testOpts;

    testOpts.addOptionChaining(
        "config", "config", moe::Int, "Config file to parse", {}, {}, OptionParserTest);
    testOpts.addOptionChaining("str", "str", moe::String, "", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.json");

    parser.setConfig("config.json", R"cfg({ str: ["val"] })cfg");

    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
}

TEST(Parsing, ScalarForListMismatch) {
    OptionsParserTester parser;
    moe::Environment environment;
    moe::OptionSection testOpts;

    testOpts.addOptionChaining(
        "config", "config", moe::Int, "Config file to parse", {}, {}, OptionParserTest);
    testOpts.addOptionChaining(
        "strlist", "strlist", moe::StringVector, "", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.json");

    parser.setConfig("config.json", R"cfg({ str: "val" })cfg");

    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
}

TEST(ConfigFromFilesystem, JSONGood) {
    moe::OptionsParser parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);
    testOpts.addOptionChaining("port", "port", moe::Int, "Port", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back(TEST_CONFIG_PATH("good.json"));

    moe::Value value;
    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("port"), &value));
    int port;
    ASSERT_OK(value.get(&port));
    ASSERT_EQUALS(port, 5);
}

TEST(ConfigFromFilesystem, INIGood) {
    moe::OptionsParser parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);
    testOpts.addOptionChaining("port", "port", moe::Int, "Port", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back(TEST_CONFIG_PATH("good.conf"));

    moe::Value value;
    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("port"), &value));
    int port;
    ASSERT_OK(value.get(&port));
    ASSERT_EQUALS(port, 5);
}

TEST(ConfigFromFilesystem, Empty) {
    moe::OptionsParser parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back(TEST_CONFIG_PATH("empty.json"));

    ASSERT_OK(parser.run(testOpts, argv, &environment));
}

TEST(ConfigFromFilesystem, Directory) {
    moe::OptionsParser parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back(TEST_CONFIG_PATH(""));

    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
}

TEST(ConfigFromFilesystem, Nonexistent) {
    moe::OptionsParser parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back(TEST_CONFIG_PATH("nonexistent_file.conf"));

    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
}

TEST(ConfigFromFilesystem, NullByte) {

    moe::OptionsParser parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);
    testOpts.addOptionChaining("port", "port", moe::Int, "Port", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back(TEST_CONFIG_PATH("nullByte.conf"));

    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
}

TEST(ConfigFromFilesystem, NullSubDir) {

    moe::OptionsParser parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);
    testOpts.addOptionChaining(
        "storage.dbPath", "dbPath", moe::String, "path", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back(TEST_CONFIG_PATH("dirNullByte.conf"));

    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
}


TEST(ConfigFromFilesystem, NullTerminated) {

    moe::OptionsParser parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);
    testOpts.addOptionChaining(
        "storage.dbPath", "dbPath", moe::String, "path", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back(TEST_CONFIG_PATH("endStringNull.conf"));

    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
}

TEST(ConfigFromFilesystem, ReadRawFile) {
    std::string fileContents;
    ASSERT_OK(readRawFile(TEST_CONFIG_PATH("good.json"), &fileContents, moe::ConfigExpand{}));
    ASSERT_EQ("{\n    \"port\" : 5\n}\n", fileContents);
}

TEST(JSONConfigFile, ComposingStringVector) {
    OptionsParserTester parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);
    testOpts
        .addOptionChaining("setParameter",
                           "setParameter",
                           moe::StringVector,
                           "Multiple Values",
                           {},
                           {},
                           OptionParserTest)
        .composing();

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.json");
    argv.push_back("--setParameter");
    argv.push_back("val1");
    argv.push_back("--setParameter");
    argv.push_back("val2");

    parser.setConfig("config.json", "{ setParameter : [ \"val3\", \"val4\" ] }");

    moe::Value value;
    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("setParameter"), &value));
    std::vector<std::string> setParameter;
    std::vector<std::string>::iterator setParameterit;
    ASSERT_OK(value.get(&setParameter));
    ASSERT_EQUALS(setParameter.size(), static_cast<size_t>(4));
    setParameterit = setParameter.begin();
    ASSERT_EQUALS(*setParameterit, "val3");
    setParameterit++;
    ASSERT_EQUALS(*setParameterit, "val4");
    setParameterit++;
    ASSERT_EQUALS(*setParameterit, "val1");
    setParameterit++;
    ASSERT_EQUALS(*setParameterit, "val2");
}

TEST(JSONConfigFile, ComposingStringMap) {
    OptionsParserTester parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);
    testOpts
        .addOptionChaining("setParameter",
                           "setParameter",
                           moe::StringMap,
                           "Multiple Values",
                           {},
                           {},
                           OptionParserTest)
        .composing();

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.json");
    argv.push_back("--setParameter");
    argv.push_back("key1=value1");
    argv.push_back("--setParameter");
    argv.push_back("key2=value2");

    parser.setConfig("config.json",
                     "{ setParameter : { key2 : \"overridden_value2\", key3 : \"value3\" } }");

    moe::Value value;
    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("setParameter"), &value));
    std::map<std::string, std::string> setParameter;
    std::map<std::string, std::string>::iterator setParameterIt;
    ASSERT_OK(value.get(&setParameter));
    ASSERT_EQUALS(setParameter.size(), static_cast<size_t>(3));
    setParameterIt = setParameter.begin();
    ASSERT_EQUALS(setParameterIt->first, "key1");
    ASSERT_EQUALS(setParameterIt->second, "value1");
    setParameterIt++;
    ASSERT_EQUALS(setParameterIt->first, "key2");
    ASSERT_EQUALS(setParameterIt->second, "value2");
    setParameterIt++;
    ASSERT_EQUALS(setParameterIt->first, "key3");
    ASSERT_EQUALS(setParameterIt->second, "value3");
}

TEST(INIConfigFile, ComposingStringVector) {
    OptionsParserTester parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);
    testOpts
        .addOptionChaining("setParameter",
                           "setParameter",
                           moe::StringVector,
                           "Multiple Values",
                           {},
                           {},
                           OptionParserTest)
        .composing();

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("default.conf");
    argv.push_back("--setParameter");
    argv.push_back("val1");
    argv.push_back("--setParameter");
    argv.push_back("val2");

    parser.setConfig("default.conf", "setParameter=val3\nsetParameter=val4");

    moe::Value value;
    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("setParameter"), &value));
    std::vector<std::string> setParameter;
    std::vector<std::string>::iterator setParameterit;
    ASSERT_OK(value.get(&setParameter));
    ASSERT_EQUALS(setParameter.size(), static_cast<size_t>(4));
    setParameterit = setParameter.begin();
    ASSERT_EQUALS(*setParameterit, "val3");
    setParameterit++;
    ASSERT_EQUALS(*setParameterit, "val4");
    setParameterit++;
    ASSERT_EQUALS(*setParameterit, "val1");
    setParameterit++;
    ASSERT_EQUALS(*setParameterit, "val2");
}

TEST(INIConfigFile, ComposingStringMap) {
    OptionsParserTester parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);
    testOpts
        .addOptionChaining("setParameter",
                           "setParameter",
                           moe::StringMap,
                           "Multiple Values",
                           {},
                           {},
                           OptionParserTest)
        .composing();

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.ini");
    argv.push_back("--setParameter");
    argv.push_back("key1=value1");
    argv.push_back("--setParameter");
    argv.push_back("key2=value2");

    parser.setConfig("config.ini", "setParameter=key2=overridden_value2\nsetParameter=key3=value3");

    moe::Value value;
    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("setParameter"), &value));
    std::map<std::string, std::string> setParameter;
    std::map<std::string, std::string>::iterator setParameterIt;
    ASSERT_OK(value.get(&setParameter));
    ASSERT_EQUALS(setParameter.size(), static_cast<size_t>(3));
    setParameterIt = setParameter.begin();
    ASSERT_EQUALS(setParameterIt->first, "key1");
    ASSERT_EQUALS(setParameterIt->second, "value1");
    setParameterIt++;
    ASSERT_EQUALS(setParameterIt->first, "key2");
    ASSERT_EQUALS(setParameterIt->second, "value2");
    setParameterIt++;
    ASSERT_EQUALS(setParameterIt->first, "key3");
    ASSERT_EQUALS(setParameterIt->second, "value3");
}

TEST(YAMLConfigFile, ComposingStringVector) {
    OptionsParserTester parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);
    testOpts
        .addOptionChaining("setParameter",
                           "setParameter",
                           moe::StringVector,
                           "Multiple Values",
                           {},
                           {},
                           OptionParserTest)
        .composing();

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.yaml");
    argv.push_back("--setParameter");
    argv.push_back("val1");
    argv.push_back("--setParameter");
    argv.push_back("val2");

    parser.setConfig("config.yaml", "setParameter : \n - \"val3\"\n - \"val4\"");

    moe::Value value;
    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("setParameter"), &value));
    std::vector<std::string> setParameter;
    std::vector<std::string>::iterator setParameterit;
    ASSERT_OK(value.get(&setParameter));
    ASSERT_EQUALS(setParameter.size(), static_cast<size_t>(4));
    setParameterit = setParameter.begin();
    ASSERT_EQUALS(*setParameterit, "val3");
    setParameterit++;
    ASSERT_EQUALS(*setParameterit, "val4");
    setParameterit++;
    ASSERT_EQUALS(*setParameterit, "val1");
    setParameterit++;
    ASSERT_EQUALS(*setParameterit, "val2");
}

TEST(YAMLConfigFile, ComposingStringMap) {
    OptionsParserTester parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);
    testOpts
        .addOptionChaining("setParameter",
                           "setParameter",
                           moe::StringMap,
                           "Multiple Values",
                           {},
                           {},
                           OptionParserTest)
        .composing();

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.yaml");
    argv.push_back("--setParameter");
    argv.push_back("key1=value1");
    argv.push_back("--setParameter");
    argv.push_back("key2=value2");

    parser.setConfig("config.yaml",
                     // NOTE: Indentation is used to determine whether an option is in a sub
                     // category, so the spaces after the newlines before key2 and key3 is
                     // significant
                     "setParameter:\n key2: \"overridden_value2\"\n key3: \"value3\"");

    moe::Value value;
    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("setParameter"), &value));
    std::map<std::string, std::string> setParameter;
    std::map<std::string, std::string>::iterator setParameterIt;
    ASSERT_OK(value.get(&setParameter));
    ASSERT_EQUALS(setParameter.size(), static_cast<size_t>(3));
    setParameterIt = setParameter.begin();
    ASSERT_EQUALS(setParameterIt->first, "key1");
    ASSERT_EQUALS(setParameterIt->second, "value1");
    setParameterIt++;
    ASSERT_EQUALS(setParameterIt->first, "key2");
    ASSERT_EQUALS(setParameterIt->second, "value2");
    setParameterIt++;
    ASSERT_EQUALS(setParameterIt->first, "key3");
    ASSERT_EQUALS(setParameterIt->second, "value3");
}

TEST(LegacyInterface, Good) {
    moe::OptionsParser parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining("port", "port", moe::Int, "Port", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--port");
    argv.push_back("5");

    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_TRUE(environment.count("port"));
    try {
        int port;
        port = environment["port"].as<int>();
        ASSERT_EQUALS(port, 5);
    } catch (std::exception& e) {
        FAIL(e.what());
    }
}

TEST(LegacyInterface, NotSpecified) {
    moe::OptionsParser parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining("port", "port", moe::Int, "Port", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");

    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_FALSE(environment.count("port"));
}

TEST(LegacyInterface, BadType) {
    moe::OptionsParser parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining("port", "port", moe::Int, "Port", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--port");
    argv.push_back("5");

    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_TRUE(environment.count("port"));
    std::string port;
    try {
        port = environment["port"].as<std::string>();
        FAIL("Expected exception trying to convert int to type string");
    } catch (std::exception&) {
    }
}

TEST(ChainingInterface, GoodReference) {
    OptionsParserTester parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    // This test is to make sure our reference stays good even after we add more options.  This
    // would not be true if we were using a std::vector in our option section which may need to
    // be moved and resized.
    moe::OptionDescription& optionRef = testOpts.addOptionChaining(
        "ref", "ref", moe::String, "Save this Reference", {}, {}, OptionParserTest);
    int i;
    for (i = 0; i < 100; i++) {
        ::mongo::StringBuilder sb;
        sb << "filler" << i;
        testOpts.addOptionChaining(
            sb.str(), sb.str(), moe::String, "Filler Option", {}, {}, OptionParserTest);
    }
    moe::Value defaultVal(std::string("default"));
    moe::Value implicitVal(std::string("implicit"));
    optionRef.hidden().setDefault(defaultVal);
    optionRef.setImplicit(implicitVal);

    std::vector<moe::OptionDescription> options_vector;
    ASSERT_OK(testOpts.getAllOptions(&options_vector));

    bool foundRef = false;
    for (std::vector<moe::OptionDescription>::const_iterator iterator = options_vector.begin();
         iterator != options_vector.end();
         iterator++) {
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
    testOpts.addOptionChaining(
        "basic", "basic", moe::String, "Default Option", {}, {}, OptionParserTest);

    std::vector<moe::OptionDescription> options_vector;
    ASSERT_OK(testOpts.getAllOptions(&options_vector));

    for (std::vector<moe::OptionDescription>::const_iterator iterator = options_vector.begin();
         iterator != options_vector.end();
         iterator++) {
        if (iterator->_dottedName == "basic") {
            ASSERT_EQUALS(iterator->_singleName, "basic");
            ASSERT_EQUALS(iterator->_type, moe::String);
            ASSERT_EQUALS(iterator->_description, "Default Option");
            ASSERT_EQUALS(iterator->_isVisible, true);
            ASSERT_TRUE(iterator->_default.isEmpty());
            ASSERT_TRUE(iterator->_implicit.isEmpty());
            ASSERT_EQUALS(iterator->_isComposing, false);
        } else {
            ::mongo::StringBuilder sb;
            sb << "Found extra option: " << iterator->_dottedName << " which we did not register";
            FAIL(sb.str());
        }
    }
}

TEST(ChainingInterface, Hidden) {
    OptionsParserTester parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts
        .addOptionChaining(
            "hidden", "hidden", moe::String, "Hidden Option", {}, {}, OptionParserTest)
        .hidden();

    std::vector<moe::OptionDescription> options_vector;
    ASSERT_OK(testOpts.getAllOptions(&options_vector));

    for (std::vector<moe::OptionDescription>::const_iterator iterator = options_vector.begin();
         iterator != options_vector.end();
         iterator++) {
        if (iterator->_dottedName == "hidden") {
            ASSERT_EQUALS(iterator->_singleName, "hidden");
            ASSERT_EQUALS(iterator->_type, moe::String);
            ASSERT_EQUALS(iterator->_description, "Hidden Option");
            ASSERT_EQUALS(iterator->_isVisible, false);
            ASSERT_TRUE(iterator->_default.isEmpty());
            ASSERT_TRUE(iterator->_implicit.isEmpty());
            ASSERT_EQUALS(iterator->_isComposing, false);
        } else {
            ::mongo::StringBuilder sb;
            sb << "Found extra option: " << iterator->_dottedName << " which we did not register";
            FAIL(sb.str());
        }
    }
}

TEST(ChainingInterface, DefaultValue) {
    OptionsParserTester parser;
    moe::Environment environment;

    moe::Value defaultVal(std::string("default"));

    moe::OptionSection testOpts;
    testOpts
        .addOptionChaining("default",
                           "default",
                           moe::String,
                           "Option With Default Value",
                           {},
                           {},
                           OptionParserTest)
        .setDefault(defaultVal);

    std::vector<moe::OptionDescription> options_vector;
    ASSERT_OK(testOpts.getAllOptions(&options_vector));

    for (std::vector<moe::OptionDescription>::const_iterator iterator = options_vector.begin();
         iterator != options_vector.end();
         iterator++) {
        if (iterator->_dottedName == "default") {
            ASSERT_EQUALS(iterator->_singleName, "default");
            ASSERT_EQUALS(iterator->_type, moe::String);
            ASSERT_EQUALS(iterator->_description, "Option With Default Value");
            ASSERT_EQUALS(iterator->_isVisible, true);
            ASSERT_TRUE(iterator->_default.equal(defaultVal));
            ASSERT_TRUE(iterator->_implicit.isEmpty());
            ASSERT_EQUALS(iterator->_isComposing, false);
        } else {
            ::mongo::StringBuilder sb;
            sb << "Found extra option: " << iterator->_dottedName << " which we did not register";
            FAIL(sb.str());
        }
    }
}

TEST(ChainingInterface, ImplicitValue) {
    OptionsParserTester parser;
    moe::Environment environment;

    moe::Value implicitVal(std::string("implicit"));

    moe::OptionSection testOpts;
    testOpts
        .addOptionChaining("implicit",
                           "implicit",
                           moe::String,
                           "Option With Implicit Value",
                           {},
                           {},
                           OptionParserTest)
        .setImplicit(implicitVal);

    std::vector<moe::OptionDescription> options_vector;
    ASSERT_OK(testOpts.getAllOptions(&options_vector));

    for (std::vector<moe::OptionDescription>::const_iterator iterator = options_vector.begin();
         iterator != options_vector.end();
         iterator++) {
        if (iterator->_dottedName == "implicit") {
            ASSERT_EQUALS(iterator->_singleName, "implicit");
            ASSERT_EQUALS(iterator->_type, moe::String);
            ASSERT_EQUALS(iterator->_description, "Option With Implicit Value");
            ASSERT_EQUALS(iterator->_isVisible, true);
            ASSERT_TRUE(iterator->_default.isEmpty());
            ASSERT_TRUE(iterator->_implicit.equal(implicitVal));
            ASSERT_EQUALS(iterator->_isComposing, false);
        } else {
            ::mongo::StringBuilder sb;
            sb << "Found extra option: " << iterator->_dottedName << " which we did not register";
            FAIL(sb.str());
        }
    }
}

TEST(ChainingInterface, Composing) {
    OptionsParserTester parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts
        .addOptionChaining("setParameter",
                           "setParameter",
                           moe::StringVector,
                           "Multiple Values",
                           {},
                           {},
                           OptionParserTest)
        .composing();

    std::vector<moe::OptionDescription> options_vector;
    ASSERT_OK(testOpts.getAllOptions(&options_vector));

    for (std::vector<moe::OptionDescription>::const_iterator iterator = options_vector.begin();
         iterator != options_vector.end();
         iterator++) {
        if (iterator->_dottedName == "setParameter") {
            ASSERT_EQUALS(iterator->_singleName, "setParameter");
            ASSERT_EQUALS(iterator->_type, moe::StringVector);
            ASSERT_EQUALS(iterator->_description, "Multiple Values");
            ASSERT_EQUALS(iterator->_isVisible, true);
            ASSERT_TRUE(iterator->_default.isEmpty());
            ASSERT_TRUE(iterator->_implicit.isEmpty());
            ASSERT_EQUALS(iterator->_isComposing, true);
        } else {
            ::mongo::StringBuilder sb;
            sb << "Found extra option: " << iterator->_dottedName << " which we did not register";
            FAIL(sb.str());
        }
    }
}

TEST(ChainingInterface, Positional) {
    moe::OptionsParser parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts
        .addOptionChaining(
            "positional", "positional", moe::String, "Positional", {}, {}, OptionParserTest)
        .positional(1, 1);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("positional");

    moe::Value value;
    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("positional"), &value));
    std::string positional;
    ASSERT_OK(value.get(&positional));
    ASSERT_EQUALS(positional, "positional");
}

TEST(ChainingInterface, PositionalTooMany) {
    moe::OptionsParser parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts
        .addOptionChaining(
            "positional", "positional", moe::String, "Positional", {}, {}, OptionParserTest)
        .positional(1, 1);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("positional");
    argv.push_back("extrapositional");

    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
}

TEST(ChainingInterface, PositionalAndFlag) {
    moe::OptionsParser parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts
        .addOptionChaining(
            "positional", "positional", moe::String, "Positional", {}, {}, OptionParserTest)
        .positional(1, 1);
    testOpts.addOptionChaining("port", "port", moe::Int, "Port", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("positional");
    argv.push_back("--port");
    argv.push_back("5");

    moe::Value value;
    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("positional"), &value));
    std::string positional;
    ASSERT_OK(value.get(&positional));
    ASSERT_EQUALS(positional, "positional");
    ASSERT_OK(environment.get(moe::Key("port"), &value));
    int port;
    ASSERT_OK(value.get(&port));
    ASSERT_EQUALS(port, 5);
}

TEST(ChainingInterface, PositionalMultiple) {
    moe::OptionsParser parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts
        .addOptionChaining(
            "positional", "positional", moe::StringVector, "Positional", {}, {}, OptionParserTest)
        .positional(1, 2);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("positional1");
    argv.push_back("positional2");

    ASSERT_OK(parser.run(testOpts, argv, &environment));
    moe::Value value;
    ASSERT_OK(environment.get(moe::Key("positional"), &value));
    std::vector<std::string> positional;
    ASSERT_OK(value.get(&positional));
    std::vector<std::string>::iterator positionalit = positional.begin();
    ASSERT_EQUALS(*positionalit, "positional1");
    positionalit++;
    ASSERT_EQUALS(*positionalit, "positional2");
}

TEST(ChainingInterface, PositionalMultipleExtra) {
    moe::OptionsParser parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts
        .addOptionChaining(
            "positional", "positional", moe::StringVector, "Positional", {}, {}, OptionParserTest)
        .positional(1, 2);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("positional1");
    argv.push_back("positional2");
    argv.push_back("positional2");

    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
}

TEST(ChainingInterface, PositionalMultipleUnlimited) {
    moe::OptionsParser parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts
        .addOptionChaining(
            "positional", "positional", moe::StringVector, "Positional", {}, {}, OptionParserTest)
        .positional(1, -1);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("positional1");
    argv.push_back("positional2");
    argv.push_back("positional3");
    argv.push_back("positional4");
    argv.push_back("positional5");

    ASSERT_OK(parser.run(testOpts, argv, &environment));
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

TEST(ChainingInterface, PositionalMultipleAndFlag) {
    moe::OptionsParser parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts
        .addOptionChaining(
            "positional", "positional", moe::StringVector, "Positional", {}, {}, OptionParserTest)
        .positional(1, 2);
    testOpts.addOptionChaining("port", "port", moe::Int, "Port", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("positional1");
    argv.push_back("--port");
    argv.push_back("5");
    argv.push_back("positional2");

    ASSERT_OK(parser.run(testOpts, argv, &environment));
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

TEST(ChainingInterface, PositionalSingleMultipleUnlimitedAndFlag) {
    moe::OptionsParser parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts
        .addOptionChaining(
            "positional1", "positional1", moe::String, "Positional", {}, {}, OptionParserTest)
        .positional(1, 1);
    testOpts
        .addOptionChaining(
            "positional2", "positional2", moe::StringVector, "Positional", {}, {}, OptionParserTest)
        .positional(2, 3);
    testOpts
        .addOptionChaining(
            "positional3", "positional3", moe::StringVector, "Positional", {}, {}, OptionParserTest)
        .positional(4, -1);
    testOpts.addOptionChaining("port", "port", moe::Int, "Port", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("positional1");
    argv.push_back("positional2");
    argv.push_back("positional3");
    argv.push_back("positional4");
    argv.push_back("positional5");
    argv.push_back("positional6");
    argv.push_back("--port");
    argv.push_back("5");
    argv.push_back("positional7");
    argv.push_back("positional8");
    argv.push_back("positional9");

    moe::Value value;
    std::vector<std::string>::iterator positionalit;

    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("positional1"), &value));
    std::string positionalSingle;
    ASSERT_OK(value.get(&positionalSingle));
    ASSERT_EQUALS(positionalSingle, "positional1");

    ASSERT_OK(environment.get(moe::Key("positional2"), &value));
    std::vector<std::string> positionalMultiple;
    ASSERT_OK(value.get(&positionalMultiple));
    positionalit = positionalMultiple.begin();
    ASSERT_EQUALS(*positionalit, "positional2");
    positionalit++;
    ASSERT_EQUALS(*positionalit, "positional3");

    ASSERT_OK(environment.get(moe::Key("positional3"), &value));
    std::vector<std::string> positionalUnlimited;
    ASSERT_OK(value.get(&positionalUnlimited));
    positionalit = positionalUnlimited.begin();
    ASSERT_EQUALS(*positionalit, "positional4");
    positionalit++;
    ASSERT_EQUALS(*positionalit, "positional5");
    positionalit++;
    ASSERT_EQUALS(*positionalit, "positional6");
    positionalit++;
    ASSERT_EQUALS(*positionalit, "positional7");
    positionalit++;
    ASSERT_EQUALS(*positionalit, "positional8");
    positionalit++;
    ASSERT_EQUALS(*positionalit, "positional9");

    ASSERT_OK(environment.get(moe::Key("port"), &value));
    int port;
    ASSERT_OK(value.get(&port));
    ASSERT_EQUALS(port, 5);
}

TEST(ChainingInterface, PositionalHoleInRange) {
    moe::OptionsParser parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts
        .addOptionChaining(
            "positional1", "positional1", moe::String, "Positional", {}, {}, OptionParserTest)
        .positional(1, 1);
    testOpts
        .addOptionChaining(
            "positional3", "positional2", moe::StringVector, "Positional", {}, {}, OptionParserTest)
        .positional(3, -1);
    testOpts.addOptionChaining("port", "port", moe::Int, "Port", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");

    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
}

TEST(ChainingInterface, PositionalOverlappingRange) {
    moe::OptionsParser parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts
        .addOptionChaining(
            "positional1", "positional1", moe::String, "Positional", {}, {}, OptionParserTest)
        .positional(1, 1);
    testOpts
        .addOptionChaining(
            "positional3", "positional2", moe::StringVector, "Positional", {}, {}, OptionParserTest)
        .positional(1, 2);
    testOpts.addOptionChaining("port", "port", moe::Int, "Port", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");

    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
}

TEST(ChainingInterface, PositionalOverlappingRangeInfinite) {
    moe::OptionsParser parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts
        .addOptionChaining(
            "positional1", "positional1", moe::String, "Positional", {}, {}, OptionParserTest)
        .positional(1, 1);
    testOpts
        .addOptionChaining(
            "positional3", "positional2", moe::StringVector, "Positional", {}, {}, OptionParserTest)
        .positional(1, -1);
    testOpts.addOptionChaining("port", "port", moe::Int, "Port", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");

    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
}

TEST(ChainingInterface, PositionalMultipleInfinite) {
    moe::OptionsParser parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts
        .addOptionChaining(
            "positional1", "positional1", moe::String, "Positional", {}, {}, OptionParserTest)
        .positional(1, -1);
    testOpts
        .addOptionChaining(
            "positional3", "positional2", moe::StringVector, "Positional", {}, {}, OptionParserTest)
        .positional(3, -1);
    testOpts.addOptionChaining("port", "port", moe::Int, "Port", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");

    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
}

TEST(OptionSources, SourceCommandLine) {
    OptionsParserTester parser;
    moe::Environment environment;
    moe::Value value;
    std::vector<std::string> argv;
    std::string parameter;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);
    testOpts
        .addOptionChaining(
            "parameter", "parameter", moe::String, "Parameter", {}, {}, OptionParserTest)
        .setSources(moe::SourceCommandLine);

    argv.clear();
    argv.push_back("binaryname");
    argv.push_back("--parameter");
    argv.push_back("allowed");

    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("parameter"), &value));
    ASSERT_OK(value.get(&parameter));
    ASSERT_EQUALS(parameter, "allowed");

    argv.clear();
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.json");

    parser.setConfig("config.json", "{ parameter : \"disallowed\" }");

    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));

    argv.clear();
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.ini");

    parser.setConfig("config.ini", "parameter=disallowed");

    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
}

TEST(OptionSources, SourceINIConfig) {
    OptionsParserTester parser;
    moe::Environment environment;
    moe::Value value;
    std::vector<std::string> argv;
    std::string parameter;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);
    testOpts
        .addOptionChaining(
            "parameter", "parameter", moe::String, "Parameter", {}, {}, OptionParserTest)
        .setSources(moe::SourceINIConfig);

    argv.clear();
    argv.push_back("binaryname");
    argv.push_back("--parameter");
    argv.push_back("disallowed");

    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));

    argv.clear();
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.json");

    parser.setConfig("config.json", "{ parameter : \"disallowed\" }");

    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));

    argv.clear();
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.ini");

    parser.setConfig("config.ini", "parameter=allowed");

    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("parameter"), &value));
    ASSERT_OK(value.get(&parameter));
    ASSERT_EQUALS(parameter, "allowed");
}

TEST(OptionSources, SourceYAMLConfig) {
    OptionsParserTester parser;
    moe::Environment environment;
    moe::Value value;
    std::vector<std::string> argv;
    std::string parameter;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);
    testOpts
        .addOptionChaining(
            "parameter", "parameter", moe::String, "Parameter", {}, {}, OptionParserTest)
        .setSources(moe::SourceYAMLConfig);

    argv.clear();
    argv.push_back("binaryname");
    argv.push_back("--parameter");
    argv.push_back("disallowed");

    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));

    argv.clear();
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.json");

    parser.setConfig("config.json", "{ parameter : \"allowed\" }");

    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("parameter"), &value));
    ASSERT_OK(value.get(&parameter));
    ASSERT_EQUALS(parameter, "allowed");

    argv.clear();
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.ini");

    parser.setConfig("config.ini", "parameter=disallowed");

    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
}

TEST(OptionSources, SourceAllConfig) {
    OptionsParserTester parser;
    moe::Environment environment;
    moe::Value value;
    std::vector<std::string> argv;
    std::string parameter;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);
    testOpts
        .addOptionChaining(
            "parameter", "parameter", moe::String, "Parameter", {}, {}, OptionParserTest)
        .setSources(moe::SourceAllConfig);

    argv.clear();
    argv.push_back("binaryname");
    argv.push_back("--parameter");
    argv.push_back("disallowed");

    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));

    argv.clear();
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.json");

    parser.setConfig("config.json", "{ parameter : \"allowed\" }");

    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("parameter"), &value));
    ASSERT_OK(value.get(&parameter));
    ASSERT_EQUALS(parameter, "allowed");

    argv.clear();
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.ini");

    parser.setConfig("config.ini", "parameter=allowed");

    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("parameter"), &value));
    ASSERT_OK(value.get(&parameter));
    ASSERT_EQUALS(parameter, "allowed");
}

TEST(OptionSources, SourceAllLegacy) {
    OptionsParserTester parser;
    moe::Environment environment;
    moe::Value value;
    std::vector<std::string> argv;
    std::string parameter;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);
    testOpts
        .addOptionChaining(
            "parameter", "parameter", moe::String, "Parameter", {}, {}, OptionParserTest)
        .setSources(moe::SourceAllLegacy);

    argv.clear();
    argv.push_back("binaryname");
    argv.push_back("--parameter");
    argv.push_back("allowed");

    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("parameter"), &value));
    ASSERT_OK(value.get(&parameter));
    ASSERT_EQUALS(parameter, "allowed");

    argv.clear();
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.json");

    parser.setConfig("config.json", "{ parameter : \"disallowed\" }");

    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));

    argv.clear();
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.ini");

    parser.setConfig("config.ini", "parameter=allowed");

    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("parameter"), &value));
    ASSERT_OK(value.get(&parameter));
    ASSERT_EQUALS(parameter, "allowed");
}

TEST(OptionSources, SourceAll) {
    OptionsParserTester parser;
    moe::Environment environment;
    moe::Value value;
    std::vector<std::string> argv;
    std::string parameter;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);
    testOpts
        .addOptionChaining(
            "parameter", "parameter", moe::String, "Parameter", {}, {}, OptionParserTest)
        .setSources(moe::SourceAll);

    argv.clear();
    argv.push_back("binaryname");
    argv.push_back("--parameter");
    argv.push_back("allowed");

    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("parameter"), &value));
    ASSERT_OK(value.get(&parameter));
    ASSERT_EQUALS(parameter, "allowed");

    argv.clear();
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.json");

    parser.setConfig("config.json", "{ parameter : \"allowed\" }");

    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("parameter"), &value));
    ASSERT_OK(value.get(&parameter));
    ASSERT_EQUALS(parameter, "allowed");

    argv.clear();
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.ini");

    parser.setConfig("config.ini", "parameter=allowed");

    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("parameter"), &value));
    ASSERT_OK(value.get(&parameter));
    ASSERT_EQUALS(parameter, "allowed");
}

TEST(Constraints, MutuallyExclusiveConstraint) {
    OptionsParserTester parser;
    moe::Environment environment;
    moe::Value value;
    std::vector<std::string> argv;

    moe::OptionSection testOpts;
    testOpts
        .addOptionChaining("option1", "option1", moe::Switch, "Option1", {}, {}, OptionParserTest)
        .incompatibleWith("section.option2");
    testOpts.addOptionChaining(
        "section.option2", "option2", moe::Switch, "Option2", {}, {}, OptionParserTest);

    environment = moe::Environment();
    argv.clear();
    argv.push_back("binaryname");
    argv.push_back("--option1");
    argv.push_back("--option2");

    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_NOT_OK(environment.validate());

    environment = moe::Environment();
    argv.clear();
    argv.push_back("binaryname");
    argv.push_back("--option1");

    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.validate());
    ASSERT_OK(environment.get(moe::Key("option1"), &value));

    environment = moe::Environment();
    argv.clear();
    argv.push_back("binaryname");
    argv.push_back("--option2");

    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.validate());
    ASSERT_OK(environment.get(moe::Key("section.option2"), &value));
}

TEST(Constraints, RequiresOtherConstraint) {
    OptionsParserTester parser;
    moe::Environment environment;
    moe::Value value;
    std::vector<std::string> argv;

    moe::OptionSection testOpts;
    testOpts
        .addOptionChaining("option1", "option1", moe::Switch, "Option1", {}, {}, OptionParserTest)
        .requiresOption("section.option2");
    testOpts.addOptionChaining(
        "section.option2", "option2", moe::Switch, "Option2", {}, {}, OptionParserTest);

    environment = moe::Environment();
    argv.clear();
    argv.push_back("binaryname");
    argv.push_back("--option1");

    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_NOT_OK(environment.validate());

    environment = moe::Environment();
    argv.clear();
    argv.push_back("binaryname");
    argv.push_back("--option1");
    argv.push_back("--option2");

    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.validate());
    ASSERT_OK(environment.get(moe::Key("option1"), &value));
    ASSERT_OK(environment.get(moe::Key("section.option2"), &value));

    environment = moe::Environment();
    argv.clear();
    argv.push_back("binaryname");
    argv.push_back("--option2");

    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.validate());
    ASSERT_OK(environment.get(moe::Key("section.option2"), &value));
}

TEST(YAMLConfigFile, Basic) {
    OptionsParserTester parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);
    testOpts.addOptionChaining("port", "port", moe::Int, "Port", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.yaml");

    parser.setConfig("config.yaml", "port: 5");

    ASSERT_OK(parser.run(testOpts, argv, &environment));
    moe::Value value;
    ASSERT_OK(environment.get(moe::Key("port"), &value));
    int port;
    ASSERT_OK(value.get(&port));
    ASSERT_EQUALS(port, 5);
}

TEST(YAMLConfigFile, Empty) {
    OptionsParserTester parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.yaml");

    parser.setConfig("config.yaml", "");

    ASSERT_OK(parser.run(testOpts, argv, &environment));
}

TEST(YAMLConfigFile, Override) {
    OptionsParserTester parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);
    testOpts.addOptionChaining("port", "port", moe::Int, "Port", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.yaml");
    argv.push_back("--port");
    argv.push_back("6");


    parser.setConfig("config.yaml", "port: 5");

    ASSERT_OK(parser.run(testOpts, argv, &environment));
    moe::Value value;
    ASSERT_OK(environment.get(moe::Key("port"), &value));
    int port;
    ASSERT_OK(value.get(&port));
    ASSERT_EQUALS(port, 6);
}

TEST(YAMLConfigFile, UnregisteredOption) {
    OptionsParserTester parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.yaml");

    parser.setConfig("config.yaml", "port: 5");

    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
}

TEST(YAMLConfigFile, DuplicateOption) {
    OptionsParserTester parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);
    testOpts.addOptionChaining("port", "port", moe::Int, "Port", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.yaml");

    parser.setConfig("config.yaml", "port: 5\nport: 5");

    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
}

TEST(YAMLConfigFile, TypeChecking) {
    OptionsParserTester parser;
    moe::Environment environment;
    moe::Value value;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);
    testOpts.addOptionChaining("stringVectorVal",
                               "stringVectorVal",
                               moe::StringVector,
                               "StringVectorVal",
                               {},
                               {},
                               OptionParserTest);
    testOpts.addOptionChaining(
        "boolVal", "boolVal", moe::Bool, "BoolVal", {}, {}, OptionParserTest);
    testOpts.addOptionChaining(
        "doubleVal", "doubleVal", moe::Double, "DoubleVal", {}, {}, OptionParserTest);
    testOpts.addOptionChaining("intVal", "intVal", moe::Int, "IntVal", {}, {}, OptionParserTest);
    testOpts.addOptionChaining(
        "longVal", "longVal", moe::Long, "LongVal", {}, {}, OptionParserTest);
    testOpts.addOptionChaining(
        "stringVal", "stringVal", moe::String, "StringVal", {}, {}, OptionParserTest);
    testOpts.addOptionChaining("unsignedLongLongVal",
                               "unsignedLongLongVal",
                               moe::UnsignedLongLong,
                               "UnsignedLongLongVal",
                               {},
                               {},
                               OptionParserTest);
    testOpts.addOptionChaining(
        "unsignedVal", "unsignedVal", moe::Unsigned, "UnsignedVal", {}, {}, OptionParserTest);
    testOpts.addOptionChaining(
        "switchVal", "switchVal", moe::Switch, "SwitchVal", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.json");

    // Test StringVector type
    std::vector<std::string> stringVectorVal;

    parser.setConfig("config.json", "stringVectorVal : \"scalar\"");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
    environment = moe::Environment();
    parser.setConfig("config.json", "stringVectorVal : \"true\"");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
    environment = moe::Environment();
    parser.setConfig("config.json", "stringVectorVal : \"5\"");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
    environment = moe::Environment();
    parser.setConfig("config.json", "stringVectorVal : [ [ \"string\" ], true, 1, 1.0 ]");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));

    // The YAML parser treats everything as a string, so we just take anything that was
    // specified as a string vector type and treat it as an array of strings, even if the
    // elements are not surrounded by quotes
    environment = moe::Environment();
    parser.setConfig("config.json", "stringVectorVal : [ \"string\", bare, true, 1, 1.0 ]");
    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("stringVectorVal"), &value));
    std::vector<std::string>::iterator stringVectorValIt;
    ASSERT_OK(value.get(&stringVectorVal));
    stringVectorValIt = stringVectorVal.begin();
    ASSERT_EQUALS(*stringVectorValIt, "string");
    stringVectorValIt++;
    ASSERT_EQUALS(*stringVectorValIt, "bare");
    stringVectorValIt++;
    ASSERT_EQUALS(*stringVectorValIt, "true");
    stringVectorValIt++;
    ASSERT_EQUALS(*stringVectorValIt, "1");
    stringVectorValIt++;
    ASSERT_EQUALS(*stringVectorValIt, "1.0");

    // Test Bool type
    bool boolVal;
    environment = moe::Environment();
    parser.setConfig("config.json", "boolVal : \"lies\"");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
    environment = moe::Environment();
    parser.setConfig("config.json", "boolVal : truth");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
    environment = moe::Environment();
    parser.setConfig("config.json", "boolVal : 1");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));

    // The YAML parser treats everything as a string, so we just take anything that was
    // specified as a bool type and try to convert it to a bool, even if it was quoted
    environment = moe::Environment();
    parser.setConfig("config.json", "boolVal : \"true\"");
    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("boolVal"), &value));
    ASSERT_OK(value.get(&boolVal));
    ASSERT_EQUALS(boolVal, true);
    environment = moe::Environment();
    parser.setConfig("config.json", "boolVal : false");
    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("boolVal"), &value));
    ASSERT_OK(value.get(&boolVal));
    ASSERT_EQUALS(boolVal, false);

    // Test Double type
    double doubleVal;
    environment = moe::Environment();
    parser.setConfig("config.json", "doubleVal : \"double the monkeys\"");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
    environment = moe::Environment();
    parser.setConfig("config.json", "doubleVal : true");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));

    // The YAML parser treats everything as a string, so we just take anything that was
    // specified as a double type and try to convert it to a double, even if it was quoted
    environment = moe::Environment();
    parser.setConfig("config.json", "doubleVal : 1.5");
    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("doubleVal"), &value));
    ASSERT_OK(value.get(&doubleVal));
    ASSERT_EQUALS(doubleVal, 1.5);
    environment = moe::Environment();
    parser.setConfig("config.json", "doubleVal : -1.5");
    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("doubleVal"), &value));
    ASSERT_OK(value.get(&doubleVal));
    ASSERT_EQUALS(doubleVal, -1.5);
    environment = moe::Environment();
    parser.setConfig("config.json", "doubleVal : \"3.14\"");
    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("doubleVal"), &value));
    ASSERT_OK(value.get(&doubleVal));
    ASSERT_EQUALS(doubleVal, 3.14);
    environment = moe::Environment();
    parser.setConfig("config.json", "doubleVal : \"-3.14\"");
    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("doubleVal"), &value));
    ASSERT_OK(value.get(&doubleVal));
    ASSERT_EQUALS(doubleVal, -3.14);

    // Test Int type
    int intVal;
    environment = moe::Environment();
    parser.setConfig("config.json", "intVal : \"hungry hippos\"");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
    environment = moe::Environment();
    parser.setConfig("config.json", "intVal : 1.5");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
    environment = moe::Environment();
    parser.setConfig("config.json", "intVal : 18446744073709551617");  // 2^64 + 1
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
    environment = moe::Environment();
    parser.setConfig("config.json", "intVal : true");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));

    // The YAML parser treats everything as a string, so we just take anything that was
    // specified as an int type and try to convert it to a int, even if it was quoted
    environment = moe::Environment();
    parser.setConfig("config.json", "intVal : \"5\"");
    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("intVal"), &value));
    ASSERT_OK(value.get(&intVal));
    ASSERT_EQUALS(intVal, 5);

    environment = moe::Environment();
    parser.setConfig("config.json", "intVal : \"-5\"");
    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("intVal"), &value));
    ASSERT_OK(value.get(&intVal));
    ASSERT_EQUALS(intVal, -5);

    // Test Long type
    long longVal;
    environment = moe::Environment();
    parser.setConfig("config.json", "longVal : \"in an eating race\"");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
    environment = moe::Environment();
    parser.setConfig("config.json", "longVal : 1.5");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
    environment = moe::Environment();
    parser.setConfig("config.json", "longVal : 18446744073709551617");  // 2^64 + 1
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
    environment = moe::Environment();
    parser.setConfig("config.json", "longVal : true");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));

    // The YAML parser treats everything as a string, so we just take anything that was
    // specified as a long type and try to convert it to a long, even if it was quoted
    environment = moe::Environment();
    parser.setConfig("config.json", "longVal : \"5\"");
    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("longVal"), &value));
    ASSERT_OK(value.get(&longVal));
    ASSERT_EQUALS(longVal, 5);

    environment = moe::Environment();
    parser.setConfig("config.json", "longVal : \"-5\"");
    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("longVal"), &value));
    ASSERT_OK(value.get(&longVal));
    ASSERT_EQUALS(longVal, -5);

    // Test String type
    std::string stringVal;

    // The YAML parser treats everything as a string, so we just take anything that was
    // specified as a string type and treat it as a string, even if the element is not
    // surrounded by quotes
    environment = moe::Environment();
    parser.setConfig("config.json", "stringVal :");
    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("stringVal"), &value));
    ASSERT_OK(value.get(&stringVal));
    ASSERT_EQUALS(stringVal, "");

    environment = moe::Environment();
    parser.setConfig("config.json", "stringVal : \"1000\"");
    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("stringVal"), &value));
    ASSERT_OK(value.get(&stringVal));
    ASSERT_EQUALS(stringVal, "1000");

    environment = moe::Environment();
    parser.setConfig("config.json", "stringVal : wat man");
    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("stringVal"), &value));
    ASSERT_OK(value.get(&stringVal));
    ASSERT_EQUALS(stringVal, "wat man");

    environment = moe::Environment();
    parser.setConfig("config.json", "stringVal : true 1 string 1.0");
    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("stringVal"), &value));
    ASSERT_OK(value.get(&stringVal));
    ASSERT_EQUALS(stringVal, "true 1 string 1.0");

    // Test UnsignedLongLong type
    unsigned long long unsignedLongLongVal;
    environment = moe::Environment();
    parser.setConfig("config.json", "unsignedLongLongVal : \"unsigned hungry hippos\"");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
    environment = moe::Environment();
    parser.setConfig("config.json", "unsignedLongLongVal : 1.5");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
    environment = moe::Environment();
    parser.setConfig("config.json", "unsignedLongLongVal : 18446744073709551617");  // 2^64 + 1
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
    environment = moe::Environment();
    parser.setConfig("config.json", "unsignedLongLongVal : true");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
    environment = moe::Environment();
    parser.setConfig("config.json", "unsignedLongLongVal : \"-5\"");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));

    // The YAML parser treats everything as a string, so we just take anything that was
    // specified as an unsigned long long type and try to convert it to an unsigned long long,
    // even if it was quoted
    environment = moe::Environment();
    parser.setConfig("config.json", "unsignedLongLongVal : \"5\"");
    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("unsignedLongLongVal"), &value));
    ASSERT_OK(value.get(&unsignedLongLongVal));
    ASSERT_EQUALS(unsignedLongLongVal, 5ULL);

    // Test Unsigned type
    unsigned unsignedVal;
    environment = moe::Environment();
    parser.setConfig("config.json", "unsignedVal : \"unsigned hungry hippos\"");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
    environment = moe::Environment();
    parser.setConfig("config.json", "unsignedVal : 1.5");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
    environment = moe::Environment();
    parser.setConfig("config.json", "unsignedVal : 18446744073709551617");  // 2^64 + 1
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
    environment = moe::Environment();
    parser.setConfig("config.json", "unsignedVal : true");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
    environment = moe::Environment();
    parser.setConfig("config.json", "unsignedVal : \"-5\"");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));

    // The YAML parser treats everything as a string, so we just take anything that was
    // specified as an unsigned type and try to convert it to an unsigned, even if it was quoted
    environment = moe::Environment();
    parser.setConfig("config.json", "unsignedVal : \"5\"");
    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("unsignedVal"), &value));
    ASSERT_OK(value.get(&unsignedVal));
    ASSERT_EQUALS(unsignedVal, 5U);

    // Test Switch type
    bool switchVal;
    environment = moe::Environment();
    parser.setConfig("config.json", "switchVal : \"lies\"");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
    environment = moe::Environment();
    parser.setConfig("config.json", "switchVal : truth");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
    environment = moe::Environment();
    parser.setConfig("config.json", "switchVal : 1");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));

    // The YAML parser treats everything as a string, so we just take anything that was
    // specified as a switch type and try to convert it to a bool, even if it was quoted
    environment = moe::Environment();
    parser.setConfig("config.json", "switchVal : \"true\"");
    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("switchVal"), &value));
    ASSERT_OK(value.get(&switchVal));
    ASSERT_EQUALS(switchVal, true);
    environment = moe::Environment();
    parser.setConfig("config.json", "switchVal : false");
    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("switchVal"), &switchVal));
    ASSERT_FALSE(switchVal);
}

TEST(YAMLConfigFile, Nested) {
    OptionsParserTester parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);
    testOpts.addOptionChaining("nested.port", "port", moe::Int, "Port", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.yaml");

    parser.setConfig("config.yaml", "nested:\n    port: 5");

    ASSERT_OK(parser.run(testOpts, argv, &environment));
    moe::Value value;
    ASSERT_OK(environment.get(moe::Key("nested.port"), &value));
    int port;
    ASSERT_OK(value.get(&port));
    ASSERT_EQUALS(port, 5);
}

TEST(YAMLConfigFile, Dotted) {
    OptionsParserTester parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);
    testOpts.addOptionChaining("dotted.port", "port", moe::Int, "Port", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.yaml");

    parser.setConfig("config.yaml", "dotted.port: 5");

    ASSERT_OK(parser.run(testOpts, argv, &environment));
    moe::Value value;
    ASSERT_OK(environment.get(moe::Key("dotted.port"), &value));
    int port;
    ASSERT_OK(value.get(&port));
    ASSERT_EQUALS(port, 5);
}

TEST(YAMLConfigFile, DottedAndNested) {
    OptionsParserTester parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);
    testOpts.addOptionChaining(
        "dottednested.var1", "var1", moe::Int, "Var1", {}, {}, OptionParserTest);
    testOpts.addOptionChaining(
        "dottednested.var2", "var2", moe::Int, "Var2", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.yaml");

    parser.setConfig("config.yaml", "dottednested.var1: 5\ndottednested:\n    var2: 6");

    ASSERT_OK(parser.run(testOpts, argv, &environment));
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

// If configuration file contains a deprecated dotted name, value will be set in the
// environment with the canonical name as the key. Deprecated dotted name will not appear
// in result environment.
TEST(YAMLConfigFile, DeprecatedDottedNameDeprecatedOnly) {
    OptionsParserTester parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);
    testOpts.addOptionChaining(
        "dotted.canonical", "var1", moe::Int, "Var1", {"dotted.deprecated"}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.yaml");

    parser.setConfig("config.yaml", "dotted.deprecated: 6");

    ASSERT_OK(parser.run(testOpts, argv, &environment));
    moe::Value value;
    ASSERT_OK(environment.get(moe::Key("dotted.canonical"), &value));
    int var1;
    ASSERT_OK(value.get(&var1));
    ASSERT_EQUALS(var1, 6);
    ASSERT_FALSE(environment.count(moe::Key("dotted.deprecated")));
}

// Deprecated dotted name cannot be the same as the canonical name.
TEST(YAMLConfigFile, DeprecatedDottedNameSameAsCanonicalDottedName) {
    moe::OptionSection testOpts;
    ASSERT_THROWS(testOpts.addOptionChaining("dotted.canonical",
                                             "var1",
                                             moe::Int,
                                             "Var1",
                                             {"dotted.canonical"},
                                             {},
                                             OptionParserTest),
                  ::mongo::DBException);
}

// Deprecated dotted name cannot be the empty string.
TEST(YAMLConfigFile, DeprecatedDottedNameEmptyString) {
    moe::OptionSection testOpts;
    ASSERT_THROWS(testOpts.addOptionChaining(
                      "dotted.canonical", "var1", moe::Int, "Var1", {""}, {}, OptionParserTest),
                  ::mongo::DBException);
}

// Deprecated dotted name cannot be the same as another option's dotted name.
TEST(YAMLConfigFile, DeprecatedDottedNameSameAsOtherOptionsDottedName) {
    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "dotted.canonical1", "var1", moe::Int, "Var1", {}, {}, OptionParserTest);
    ASSERT_THROWS(testOpts.addOptionChaining("dotted.canonical2",
                                             "var2",
                                             moe::Int,
                                             "Var2",
                                             {"dotted.canonical1"},
                                             {},
                                             OptionParserTest),
                  ::mongo::DBException);
}

// Deprecated dotted name cannot be the same as another option's deprecated dotted name.
TEST(YAMLConfigFile, DeprecatedDottedNameSameAsOtherOptionsDeprecatedDottedName) {
    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "dotted.canonical1", "var1", moe::Int, "Var1", {"dotted.deprecated"}, {}, OptionParserTest);
    ASSERT_THROWS(testOpts.addOptionChaining("dotted.canonical2",
                                             "var2",
                                             moe::Int,
                                             "Var2",
                                             {"dotted.deprecated"},
                                             {},
                                             OptionParserTest),
                  ::mongo::DBException);
}

// It is an error to have both canonical and deprecated dotted names in the same
// configuration file.
TEST(YAMLConfigFile, DeprecatedDottedNameCanonicalAndDeprecated) {
    OptionsParserTester parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);
    testOpts.addOptionChaining(
        "dotted.canonical", "var1", moe::Int, "Var1", {"dotted.deprecated"}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.yaml");

    parser.setConfig("config.yaml",
                     "dotted.canonical: 5\n"
                     "dotted.deprecated: 6");

    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
}

// An option can have multiple deprecated dotted names.
TEST(YAMLConfigFile, DeprecatedDottedNameMultipleDeprecated) {
    std::vector<std::string> deprecatedDottedNames;
    deprecatedDottedNames.push_back("dotted.deprecated1");
    deprecatedDottedNames.push_back("dotted.deprecated2");

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);
    testOpts.addOptionChaining(
        "dotted.canonical", "var1", moe::Int, "Var1", deprecatedDottedNames, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.yaml");

    // Parse 2 files - each containing a different deprecated dotted name.
    for (std::vector<std::string>::const_iterator i = deprecatedDottedNames.begin();
         i != deprecatedDottedNames.end();
         ++i) {
        OptionsParserTester parser;
        moe::Environment environment;

        ::mongo::StringBuilder sb;
        sb << *i << ": 6";
        parser.setConfig("config.yaml", sb.str());

        ASSERT_OK(parser.run(testOpts, argv, &environment));
        moe::Value value;
        ASSERT_OK(environment.get(moe::Key("dotted.canonical"), &value));
        int var1;
        ASSERT_OK(value.get(&var1));
        ASSERT_EQUALS(var1, 6);
        ASSERT_FALSE(environment.count(moe::Key(deprecatedDottedNames[0])));
        ASSERT_FALSE(environment.count(moe::Key(deprecatedDottedNames[1])));
    }

    // It is an error to have multiple deprecated dotted names mapping to the same option
    // in the same file.
    {
        OptionsParserTester parser;
        moe::Environment environment;

        std::stringstream ss;
        ss << deprecatedDottedNames[0] << ": 6" << std::endl << deprecatedDottedNames[1] << ": 7";
        parser.setConfig("config.yaml", ss.str());

        ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
    }
}

TEST(YAMLConfigFile, ListBrackets) {
    OptionsParserTester parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);
    testOpts.addOptionChaining(
        "multival", "multival", moe::StringVector, "Multiple Values", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.yaml");

    parser.setConfig("config.yaml", "multival: [ \"val1\", \"val2\" ]");

    moe::Value value;
    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("multival"), &value));
    std::vector<std::string> multival;
    std::vector<std::string>::iterator multivalit;
    ASSERT_OK(value.get(&multival));
    multivalit = multival.begin();
    ASSERT_EQUALS(*multivalit, "val1");
    multivalit++;
    ASSERT_EQUALS(*multivalit, "val2");
}

TEST(YAMLConfigFile, ListDashes) {
    OptionsParserTester parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);
    testOpts.addOptionChaining(
        "multival", "multival", moe::StringVector, "Multiple Values", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.yaml");

    parser.setConfig("config.yaml", "multival:\n - \"val1\"\n - \"val2\"");

    moe::Value value;
    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("multival"), &value));
    std::vector<std::string> multival;
    std::vector<std::string>::iterator multivalit;
    ASSERT_OK(value.get(&multival));
    multivalit = multival.begin();
    ASSERT_EQUALS(*multivalit, "val1");
    multivalit++;
    ASSERT_EQUALS(*multivalit, "val2");
}

TEST(YAMLConfigFile, DefaultValueOverride) {
    OptionsParserTester parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);
    testOpts.addOptionChaining("port", "port", moe::Int, "Port", {}, {}, OptionParserTest)
        .setDefault(moe::Value(5));

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.yaml");

    parser.setConfig("config.yaml", "port: 6");

    ASSERT_OK(parser.run(testOpts, argv, &environment));
    moe::Value value;
    ASSERT_OK(environment.get(moe::Key("port"), &value));
    int port;
    ASSERT_OK(value.get(&port));
    ASSERT_EQUALS(port, 6);
}

TEST(YAMLConfigFile, Comments) {
    OptionsParserTester parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);
    testOpts.addOptionChaining("port", "port", moe::Int, "Port", {}, {}, OptionParserTest);
    testOpts.addOptionChaining("host", "host", moe::String, "Host", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.yaml");

    parser.setConfig("config.yaml",
                     "# comment on port\nport: 5\n"
                     "# comment on host\nhost: localhost\n");

    moe::Value value;
    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("port"), &value));
    int port;
    ASSERT_OK(value.get(&port));
    ASSERT_EQUALS(port, 5);
    ASSERT_OK(environment.get(moe::Key("host"), &value));
    std::string host;
    ASSERT_OK(value.get(&host));
    ASSERT_EQUALS(host, "localhost");
}

TEST(YAMLConfigFile, EmptyKey) {
    OptionsParserTester parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);
    testOpts.addOptionChaining("port", "port", moe::Int, "Port", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.yaml");

    parser.setConfig("config.yaml", ":");

    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
}

TEST(YAMLConfigFile, StringVector) {
    OptionsParserTester parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);
    testOpts.addOptionChaining(
        "multival", "multival", moe::StringVector, "Multiple Values", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.json");

    parser.setConfig("config.json", "multival : [ \"val1\", \"val2\" ]");

    moe::Value value;
    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("multival"), &value));
    std::vector<std::string> multival;
    std::vector<std::string>::iterator multivalit;
    ASSERT_OK(value.get(&multival));
    multivalit = multival.begin();
    ASSERT_EQUALS(*multivalit, "val1");
    multivalit++;
    ASSERT_EQUALS(*multivalit, "val2");
}

TEST(YAMLConfigFile, StringMap) {
    OptionsParserTester parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);
    testOpts.addOptionChaining(
        "multival", "multival", moe::StringMap, "Multiple Values", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.json");

    parser.setConfig("config.json",
                     // NOTE: Indentation is used to determine whether an option is in a sub
                     // category, so the spaces after the newlines before key2 and key3 is
                     // significant
                     "multival : \n key1 : \"value1\"\n key2 : \"value2\"\n key3 : \"\"");

    moe::Value value;
    ASSERT_OK(parser.run(testOpts, argv, &environment));
    ASSERT_OK(environment.get(moe::Key("multival"), &value));
    std::map<std::string, std::string> multival;
    std::map<std::string, std::string>::iterator multivalit;
    ASSERT_OK(value.get(&multival));
    multivalit = multival.begin();
    ASSERT_EQUALS(multivalit->first, "key1");
    ASSERT_EQUALS(multivalit->second, "value1");
    multivalit++;
    ASSERT_EQUALS(multivalit->first, "key2");
    ASSERT_EQUALS(multivalit->second, "value2");
    multivalit++;
    ASSERT_EQUALS(multivalit->first, "key3");
    ASSERT_EQUALS(multivalit->second, "");
}

TEST(YAMLConfigFile, StringMapDuplicateKey) {
    OptionsParserTester parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);
    testOpts.addOptionChaining(
        "multival", "multival", moe::StringMap, "Multiple Values", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.json");

    parser.setConfig("config.json",
                     // NOTE: Indentation is used to determine whether an option is in a sub
                     // category, so the spaces after the newlines before key2 and key3 is
                     // significant
                     "multival : \n key1 : \"value1\"\n key1 : \"value2\"");

    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
}

TEST(OptionCount, Basic) {
    OptionsParserTester parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "basic", "basic", moe::String, "Basic Option", {}, {}, OptionParserTest);
    testOpts
        .addOptionChaining(
            "hidden", "hidden", moe::String, "Hidden Option", {}, {}, OptionParserTest)
        .hidden();

    moe::OptionSection subSection("Section Name");
    subSection.addOptionChaining("port", "port", moe::Int, "Port", {}, {}, OptionParserTest)
        .setSources(moe::SourceYAMLConfig);
    ASSERT_OK(testOpts.addSection(subSection));

    int numOptions;
    ASSERT_OK(testOpts.countOptions(&numOptions, true /*visibleOnly*/, moe::SourceCommandLine));
    ASSERT_EQUALS(numOptions, 1);
}

TEST(NumericalBaseParsing, CommandLine) {
    OptionsParserTester parser;
    moe::Environment environment;
    moe::Value value;
    std::vector<std::string> argv;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "doubleVal", "doubleVal", moe::Double, "DoubleVal", {}, {}, OptionParserTest);
    testOpts.addOptionChaining("intVal", "intVal", moe::Int, "IntVal", {}, {}, OptionParserTest);
    testOpts.addOptionChaining(
        "longVal", "longVal", moe::Long, "LongVal", {}, {}, OptionParserTest);
    testOpts.addOptionChaining("unsignedLongLongVal",
                               "unsignedLongLongVal",
                               moe::UnsignedLongLong,
                               "UnsignedLongLongVal",
                               {},
                               {},
                               OptionParserTest);
    testOpts.addOptionChaining(
        "unsignedVal", "unsignedVal", moe::Unsigned, "UnsignedVal", {}, {}, OptionParserTest);

    // Bad values
    argv = std::vector<std::string>();
    argv.push_back("binaryname");
    argv.push_back("--doubleVal");
    argv.push_back("monkeys");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));

    argv = std::vector<std::string>();
    argv.push_back("binaryname");
    argv.push_back("--intVal");
    argv.push_back("monkeys");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));

    argv = std::vector<std::string>();
    argv.push_back("binaryname");
    argv.push_back("--longVal");
    argv.push_back("monkeys");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));

    argv = std::vector<std::string>();
    argv.push_back("binaryname");
    argv.push_back("--unsignedLongLongVal");
    argv.push_back("monkeys");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));

    argv = std::vector<std::string>();
    argv.push_back("binaryname");
    argv.push_back("--unsignedVal");
    argv.push_back("monkeys");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));

    // Decimal values
    argv = std::vector<std::string>();
    argv.push_back("binaryname");
    argv.push_back("--doubleVal");
    argv.push_back("16.1");
    argv.push_back("--intVal");
    argv.push_back("16");
    argv.push_back("--longVal");
    argv.push_back("16");
    argv.push_back("--unsignedLongLongVal");
    argv.push_back("16");
    argv.push_back("--unsignedVal");
    argv.push_back("16");
    environment = moe::Environment();
    ASSERT_OK(parser.run(testOpts, argv, &environment));

    double doubleVal;
    int intVal;
    long longVal;
    unsigned long long unsignedLongLongVal;
    unsigned unsignedVal;

    ASSERT_OK(environment.get(moe::Key("doubleVal"), &value));
    ASSERT_OK(value.get(&doubleVal));
    ASSERT_EQUALS(doubleVal, 16.1);

    ASSERT_OK(environment.get(moe::Key("intVal"), &value));
    ASSERT_OK(value.get(&intVal));
    ASSERT_EQUALS(intVal, 16);

    ASSERT_OK(environment.get(moe::Key("longVal"), &value));
    ASSERT_OK(value.get(&longVal));
    ASSERT_EQUALS(longVal, 16);

    ASSERT_OK(environment.get(moe::Key("unsignedLongLongVal"), &value));
    ASSERT_OK(value.get(&unsignedLongLongVal));
    ASSERT_EQUALS(unsignedLongLongVal, 16ULL);

    ASSERT_OK(environment.get(moe::Key("unsignedVal"), &value));
    ASSERT_OK(value.get(&unsignedVal));
    ASSERT_EQUALS(unsignedVal, 16U);

    // Octal values
    argv = std::vector<std::string>();
    argv.push_back("binaryname");
    argv.push_back("--doubleVal");
    argv.push_back("020.1");
    argv.push_back("--intVal");
    argv.push_back("020");
    argv.push_back("--longVal");
    argv.push_back("020");
    argv.push_back("--unsignedLongLongVal");
    argv.push_back("020");
    argv.push_back("--unsignedVal");
    argv.push_back("020");
    environment = moe::Environment();
    ASSERT_OK(parser.run(testOpts, argv, &environment));

    ASSERT_OK(environment.get(moe::Key("doubleVal"), &value));
    ASSERT_OK(value.get(&doubleVal));
    ASSERT_EQUALS(doubleVal, 020.1);

    ASSERT_OK(environment.get(moe::Key("intVal"), &value));
    ASSERT_OK(value.get(&intVal));
    ASSERT_EQUALS(intVal, 020);

    ASSERT_OK(environment.get(moe::Key("longVal"), &value));
    ASSERT_OK(value.get(&longVal));
    ASSERT_EQUALS(longVal, 020);

    ASSERT_OK(environment.get(moe::Key("unsignedLongLongVal"), &value));
    ASSERT_OK(value.get(&unsignedLongLongVal));
    ASSERT_EQUALS(unsignedLongLongVal, 020ULL);

    ASSERT_OK(environment.get(moe::Key("unsignedVal"), &value));
    ASSERT_OK(value.get(&unsignedVal));
    ASSERT_EQUALS(unsignedVal, 020U);

    // Hex values
    argv = std::vector<std::string>();
    argv.push_back("binaryname");
#if !defined(_WIN32)
    // Hex doubles are not parseable by the Windows SDK libc or the Solaris libc in the mode we
    // build, so we cannot read hex doubles from the command line on those platforms.
    // See SERVER-14131.

    argv.push_back("--doubleVal");
    argv.push_back("0x10.1");
#endif
    argv.push_back("--intVal");
    argv.push_back("0x10");
    argv.push_back("--longVal");
    argv.push_back("0x10");
    argv.push_back("--unsignedLongLongVal");
    argv.push_back("0x10");
    argv.push_back("--unsignedVal");
    argv.push_back("0x10");
    environment = moe::Environment();
    ASSERT_OK(parser.run(testOpts, argv, &environment));

#if !defined(_WIN32)
    // See SERVER-14131.
    ASSERT_OK(environment.get(moe::Key("doubleVal"), &value));
    ASSERT_OK(value.get(&doubleVal));
    ASSERT_EQUALS(doubleVal, 0x10.1p0);
#endif

    ASSERT_OK(environment.get(moe::Key("intVal"), &value));
    ASSERT_OK(value.get(&intVal));
    ASSERT_EQUALS(intVal, 0x10);

    ASSERT_OK(environment.get(moe::Key("longVal"), &value));
    ASSERT_OK(value.get(&longVal));
    ASSERT_EQUALS(longVal, 0x10);

    ASSERT_OK(environment.get(moe::Key("unsignedLongLongVal"), &value));
    ASSERT_OK(value.get(&unsignedLongLongVal));
    ASSERT_EQUALS(unsignedLongLongVal, 0x10ULL);

    ASSERT_OK(environment.get(moe::Key("unsignedVal"), &value));
    ASSERT_OK(value.get(&unsignedVal));
    ASSERT_EQUALS(unsignedVal, 0x10U);
}

TEST(NumericalBaseParsing, INIConfigFile) {
    OptionsParserTester parser;
    moe::Environment environment;
    moe::Value value;
    std::vector<std::string> argv;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);
    testOpts.addOptionChaining(
        "doubleVal", "doubleVal", moe::Double, "DoubleVal", {}, {}, OptionParserTest);
    testOpts.addOptionChaining("intVal", "intVal", moe::Int, "IntVal", {}, {}, OptionParserTest);
    testOpts.addOptionChaining(
        "longVal", "longVal", moe::Long, "LongVal", {}, {}, OptionParserTest);
    testOpts.addOptionChaining("unsignedLongLongVal",
                               "unsignedLongLongVal",
                               moe::UnsignedLongLong,
                               "UnsignedLongLongVal",
                               {},
                               {},
                               OptionParserTest);
    testOpts.addOptionChaining(
        "unsignedVal", "unsignedVal", moe::Unsigned, "UnsignedVal", {}, {}, OptionParserTest);

    // Bad values
    argv = std::vector<std::string>();
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.ini");

    parser.setConfig("config.ini", "doubleVal=monkeys");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));

    parser.setConfig("config.ini", "intVal=monkeys");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));

    parser.setConfig("config.ini", "longVal=monkeys");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));

    parser.setConfig("config.ini", "unsignedLongLongVal=monkeys");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));

    parser.setConfig("config.ini", "unsignedVal=monkeys");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));

    // Decimal values
    argv = std::vector<std::string>();
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.ini");
    parser.setConfig("config.ini",
                     "doubleVal=16.1\nintVal=16\nlongVal=16\n"
                     "unsignedLongLongVal=16\nunsignedVal=16\n");
    environment = moe::Environment();
    ASSERT_OK(parser.run(testOpts, argv, &environment));

    double doubleVal;
    int intVal;
    long longVal;
    unsigned long long unsignedLongLongVal;
    unsigned unsignedVal;

    ASSERT_OK(environment.get(moe::Key("doubleVal"), &value));
    ASSERT_OK(value.get(&doubleVal));
    ASSERT_EQUALS(doubleVal, 16.1);

    ASSERT_OK(environment.get(moe::Key("intVal"), &value));
    ASSERT_OK(value.get(&intVal));
    ASSERT_EQUALS(intVal, 16);

    ASSERT_OK(environment.get(moe::Key("longVal"), &value));
    ASSERT_OK(value.get(&longVal));
    ASSERT_EQUALS(longVal, 16);

    ASSERT_OK(environment.get(moe::Key("unsignedLongLongVal"), &value));
    ASSERT_OK(value.get(&unsignedLongLongVal));
    ASSERT_EQUALS(unsignedLongLongVal, 16ULL);

    ASSERT_OK(environment.get(moe::Key("unsignedVal"), &value));
    ASSERT_OK(value.get(&unsignedVal));
    ASSERT_EQUALS(unsignedVal, 16U);

    // Octal values
    argv = std::vector<std::string>();
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.ini");
    parser.setConfig("config.ini",
                     "doubleVal=020.1\nintVal=020\nlongVal=020\n"
                     "unsignedLongLongVal=020\nunsignedVal=020\n");
    environment = moe::Environment();
    ASSERT_OK(parser.run(testOpts, argv, &environment));

    ASSERT_OK(environment.get(moe::Key("doubleVal"), &value));
    ASSERT_OK(value.get(&doubleVal));
    ASSERT_EQUALS(doubleVal, 020.1);

    ASSERT_OK(environment.get(moe::Key("intVal"), &value));
    ASSERT_OK(value.get(&intVal));
    ASSERT_EQUALS(intVal, 020);

    ASSERT_OK(environment.get(moe::Key("longVal"), &value));
    ASSERT_OK(value.get(&longVal));
    ASSERT_EQUALS(longVal, 020);

    ASSERT_OK(environment.get(moe::Key("unsignedLongLongVal"), &value));
    ASSERT_OK(value.get(&unsignedLongLongVal));
    ASSERT_EQUALS(unsignedLongLongVal, 020ULL);

    ASSERT_OK(environment.get(moe::Key("unsignedVal"), &value));
    ASSERT_OK(value.get(&unsignedVal));
    ASSERT_EQUALS(unsignedVal, 020U);

    // Hex values
    argv = std::vector<std::string>();
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.ini");
#if !defined(_WIN32)
    // Hex doubles are not parseable by the Windows SDK libc or the Solaris libc in the mode we
    // build, so we cannot read hex doubles from a config file on those platforms.
    // See SERVER-14131.

    parser.setConfig("config.ini",
                     "doubleVal=0x10.1\nintVal=0x10\nlongVal=0x10\n"
                     "unsignedLongLongVal=0x10\nunsignedVal=0x10\n");
#else
    parser.setConfig("config.ini",
                     "intVal=0x10\nlongVal=0x10\n"
                     "unsignedLongLongVal=0x10\nunsignedVal=0x10\n");
#endif
    environment = moe::Environment();
    ASSERT_OK(parser.run(testOpts, argv, &environment));

#if !defined(_WIN32)
    // See SERVER-14131.
    ASSERT_OK(environment.get(moe::Key("doubleVal"), &value));
    ASSERT_OK(value.get(&doubleVal));
    ASSERT_EQUALS(doubleVal, 0x10.1p0);
#endif

    ASSERT_OK(environment.get(moe::Key("intVal"), &value));
    ASSERT_OK(value.get(&intVal));
    ASSERT_EQUALS(intVal, 0x10);

    ASSERT_OK(environment.get(moe::Key("longVal"), &value));
    ASSERT_OK(value.get(&longVal));
    ASSERT_EQUALS(longVal, 0x10);

    ASSERT_OK(environment.get(moe::Key("unsignedLongLongVal"), &value));
    ASSERT_OK(value.get(&unsignedLongLongVal));
    ASSERT_EQUALS(unsignedLongLongVal, 0x10ULL);

    ASSERT_OK(environment.get(moe::Key("unsignedVal"), &value));
    ASSERT_OK(value.get(&unsignedVal));
    ASSERT_EQUALS(unsignedVal, 0x10U);
}

TEST(NumericalBaseParsing, YAMLConfigFile) {
    OptionsParserTester parser;
    moe::Environment environment;
    moe::Value value;
    std::vector<std::string> argv;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);
    testOpts.addOptionChaining(
        "doubleVal", "doubleVal", moe::Double, "DoubleVal", {}, {}, OptionParserTest);
    testOpts.addOptionChaining("intVal", "intVal", moe::Int, "IntVal", {}, {}, OptionParserTest);
    testOpts.addOptionChaining(
        "longVal", "longVal", moe::Long, "LongVal", {}, {}, OptionParserTest);
    testOpts.addOptionChaining("unsignedLongLongVal",
                               "unsignedLongLongVal",
                               moe::UnsignedLongLong,
                               "UnsignedLongLongVal",
                               {},
                               {},
                               OptionParserTest);
    testOpts.addOptionChaining(
        "unsignedVal", "unsignedVal", moe::Unsigned, "UnsignedVal", {}, {}, OptionParserTest);

    // Bad values
    argv = std::vector<std::string>();
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.yaml");

    parser.setConfig("config.yaml", "doubleVal: \"monkeys\"");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));

    parser.setConfig("config.yaml", "intVal: \"monkeys\"");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));

    parser.setConfig("config.yaml", "longVal: \"monkeys\"");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));

    parser.setConfig("config.yaml", "unsignedLongLongVal: \"monkeys\"");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));

    parser.setConfig("config.yaml", "unsignedVal: \"monkeys\"");
    ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));

    // Decimal values
    argv = std::vector<std::string>();
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.yaml");
    parser.setConfig("config.yaml",
                     "doubleVal: 16.1\nintVal: 16\nlongVal: 16\n"
                     "unsignedLongLongVal: 16\nunsignedVal: 16\n");
    environment = moe::Environment();
    ASSERT_OK(parser.run(testOpts, argv, &environment));

    double doubleVal;
    int intVal;
    long longVal;
    unsigned long long unsignedLongLongVal;
    unsigned unsignedVal;

    ASSERT_OK(environment.get(moe::Key("doubleVal"), &value));
    ASSERT_OK(value.get(&doubleVal));
    ASSERT_EQUALS(doubleVal, 16.1);

    ASSERT_OK(environment.get(moe::Key("intVal"), &value));
    ASSERT_OK(value.get(&intVal));
    ASSERT_EQUALS(intVal, 16);

    ASSERT_OK(environment.get(moe::Key("longVal"), &value));
    ASSERT_OK(value.get(&longVal));
    ASSERT_EQUALS(longVal, 16);

    ASSERT_OK(environment.get(moe::Key("unsignedLongLongVal"), &value));
    ASSERT_OK(value.get(&unsignedLongLongVal));
    ASSERT_EQUALS(unsignedLongLongVal, 16ULL);

    ASSERT_OK(environment.get(moe::Key("unsignedVal"), &value));
    ASSERT_OK(value.get(&unsignedVal));
    ASSERT_EQUALS(unsignedVal, 16U);

    // Octal values
    argv = std::vector<std::string>();
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.yaml");
    parser.setConfig("config.yaml",
                     "doubleVal: 020.1\nintVal: 020\nlongVal: 020\n"
                     "unsignedLongLongVal: 020\nunsignedVal: 020\n");
    environment = moe::Environment();
    ASSERT_OK(parser.run(testOpts, argv, &environment));

    ASSERT_OK(environment.get(moe::Key("doubleVal"), &value));
    ASSERT_OK(value.get(&doubleVal));
    ASSERT_EQUALS(doubleVal, 020.1);

    ASSERT_OK(environment.get(moe::Key("intVal"), &value));
    ASSERT_OK(value.get(&intVal));
    ASSERT_EQUALS(intVal, 020);

    ASSERT_OK(environment.get(moe::Key("longVal"), &value));
    ASSERT_OK(value.get(&longVal));
    ASSERT_EQUALS(longVal, 020);

    ASSERT_OK(environment.get(moe::Key("unsignedLongLongVal"), &value));
    ASSERT_OK(value.get(&unsignedLongLongVal));
    ASSERT_EQUALS(unsignedLongLongVal, 020ULL);

    ASSERT_OK(environment.get(moe::Key("unsignedVal"), &value));
    ASSERT_OK(value.get(&unsignedVal));
    ASSERT_EQUALS(unsignedVal, 020U);

    // Hex values
    argv = std::vector<std::string>();
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back("config.yaml");
#if !defined(_WIN32)
    // Hex doubles are not parseable by the Windows SDK libc or the Solaris libc in the mode we
    // build, so we cannot read hex doubles from a config file on those platforms.
    // See SERVER-14131.

    parser.setConfig("config.yaml",
                     "doubleVal: 0x10.1\nintVal: 0x10\nlongVal: 0x10\n"
                     "unsignedLongLongVal: 0x10\nunsignedVal: 0x10\n");
#else
    parser.setConfig("config.yaml",
                     "intVal: 0x10\nlongVal: 0x10\n"
                     "unsignedLongLongVal: 0x10\nunsignedVal: 0x10\n");
#endif
    environment = moe::Environment();
    ASSERT_OK(parser.run(testOpts, argv, &environment));

#if !defined(_WIN32)
    // See SERVER-14131.
    ASSERT_OK(environment.get(moe::Key("doubleVal"), &value));
    ASSERT_OK(value.get(&doubleVal));
    ASSERT_EQUALS(doubleVal, 0x10.1p0);
#endif

    ASSERT_OK(environment.get(moe::Key("intVal"), &value));
    ASSERT_OK(value.get(&intVal));
    ASSERT_EQUALS(intVal, 0x10);

    ASSERT_OK(environment.get(moe::Key("longVal"), &value));
    ASSERT_OK(value.get(&longVal));
    ASSERT_EQUALS(longVal, 0x10);

    ASSERT_OK(environment.get(moe::Key("unsignedLongLongVal"), &value));
    ASSERT_OK(value.get(&unsignedLongLongVal));
    ASSERT_EQUALS(unsignedLongLongVal, 0x10ULL);

    ASSERT_OK(environment.get(moe::Key("unsignedVal"), &value));
    ASSERT_OK(value.get(&unsignedVal));
    ASSERT_EQUALS(unsignedVal, 0x10U);
}

TEST(YAMLConfigFile, OutputConfig) {
    moe::OptionSection options;
    options.addOptionChaining("cacheSize", "cacheSize", moe::Long, "", {}, {}, OptionParserTest);
    options.addOptionChaining(
        "command", "command", moe::StringVector, "", {}, {}, OptionParserTest);
    options.addOptionChaining("config", "config", moe::String, "", {}, {}, OptionParserTest);
    options.addOptionChaining("math.pi", "pi", moe::Double, "", {}, {}, OptionParserTest);
    options.addOptionChaining("net.port", "port", moe::Int, "", {}, {}, OptionParserTest);
    options.addOptionChaining("net.bindIp", "bind_ip", moe::String, "", {}, {}, OptionParserTest);
    options.addOptionChaining(
        "net.bindIpAll", "bind_ip_all", moe::Switch, "", {}, {}, OptionParserTest);
    options.addOptionChaining(
        "security.javascriptEnabled", "javascriptEnabled", moe::Bool, "", {}, {}, OptionParserTest);
    options.addOptionChaining(
        "setParameter", "setParameter", moe::StringMap, "", {}, {}, OptionParserTest);
    options.addOptionChaining(
        "systemLog.path", "logPath", moe::String, "", {}, {}, OptionParserTest);

    OptionsParserTester parser;
    parser.setConfig("config.yaml",
                     "systemLog: { path: /tmp/mongod.log }\n"
                     "command: [ mongo, mongod, mongos ]");

    const std::vector<std::string> argv = {
        "binaryname",
        "--port",
        "31337",
        "--bind_ip",
        "127.0.0.1,::1",
        "--bind_ip_all",
        "--setParameter",
        "scramSHAIterationCount=12345",
        "--javascriptEnabled",
        "false",
        "--cacheSize",
        "12345",
        "--pi",
        "3.14159265",
        "--config",
        "config.yaml",
    };

    moe::Environment env;
    ASSERT_OK(parser.run(options, argv, &env));
    ASSERT_EQ(env.toYAML(),
              "cacheSize: 12345\n"
              "command:\n"
              "  - mongo\n"
              "  - mongod\n"
              "  - mongos\n"
              "config: config.yaml\n"
              "math:\n"
              "  pi: 3.14159265\n"
              "net:\n"
              "  bindIp: 127.0.0.1,::1\n"
              "  bindIpAll: true\n"
              "  port: 31337\n"
              "security:\n"
              "  javascriptEnabled: false\n"
              "setParameter:\n"
              "  scramSHAIterationCount: 12345\n"
              "systemLog:\n"
              "  path: /tmp/mongod.log");
}

void TestFile(std::vector<unsigned char> contents, bool valid) {
    mongo::unittest::TempDir tempdir("options_testpath");
    boost::filesystem::path p(tempdir.path());
    p /= "config.yaml";

    {
        std::ofstream ofs(p.generic_string(), std::ios::binary);
        ofs.write(reinterpret_cast<char*>(contents.data()), contents.size());
    }

    moe::OptionsParser parser;
    moe::Environment environment;

    moe::OptionSection testOpts;
    testOpts.addOptionChaining(
        "config", "config", moe::String, "Config file to parse", {}, {}, OptionParserTest);
    testOpts.addOptionChaining("port", "port", moe::Int, "Port", {}, {}, OptionParserTest);

    std::vector<std::string> argv;
    argv.push_back("binaryname");
    argv.push_back("--config");
    argv.push_back(p.generic_string());

    if (valid) {
        ASSERT_OK(parser.run(testOpts, argv, &environment));

        moe::Value value;
        ASSERT_OK(environment.get(moe::Key("port"), &value));
        int port;
        ASSERT_OK(value.get(&port));
        ASSERT_EQUALS(port, 1234);
    } else {
        ASSERT_NOT_OK(parser.run(testOpts, argv, &environment));
    }
}

TEST(YAMLConfigFile, canonicalize) {
    moe::OptionSection opts;
    opts.addOptionChaining("net.bindIpAll",
                           "bind_ip_all",
                           moe::Switch,
                           "Bind all addresses",
                           {},
                           {},
                           OptionParserTest)
        .incompatibleWith("net.bindIp")
        .canonicalize([](moe::Environment* env) {
            auto status = env->remove("net.bindIpAll");
            if (!status.isOK()) {
                return status;
            }
            return env->set("net.bindIp", moe::Value("0.0.0.0"));
        });
    opts.addOptionChaining("net.bindIp",
                           "bind_ip",
                           moe::String,
                           "Bind specific addresses",
                           {},
                           {},
                           OptionParserTest)
        .incompatibleWith("net.bindIpAll");

    moe::OptionsParser parser;
    moe::Environment env;
    std::vector<std::string> argv = {
        "binary",
        "--bind_ip_all",
    };
    ASSERT_OK(parser.run(opts, argv, &env));
    ASSERT_TRUE(env.count("net.bindIp"));
    ASSERT_FALSE(env.count("net.bindIpAll"));
    ASSERT_EQ(env["net.bindIp"].as<std::string>(), "0.0.0.0");
}

#if defined(_WIN32)
// Positive: Validate a UTF-16 file with a BOM can be parsed
TEST(YAMLConfigFile, UTF16WithBOMFile) {
    // This array represents a file with a UTF-16 LE BOM and the contents:
    // port: 1234
    // <blank line>
    std::vector<unsigned char> data{0xff, 0xfe, 0x70, 0x00, 0x6f, 0x00, 0x72, 0x00, 0x74,
                                    0x00, 0x3a, 0x00, 0x20, 0x00, 0x31, 0x00, 0x32, 0x00,
                                    0x33, 0x00, 0x34, 0x00, 0x0d, 0x00, 0x0a, 0x00};
    TestFile(data, true);
}

// Negative: Validate a UTF-16 file without a BOM cannot be parsed
TEST(YAMLConfigFile, UTF16WithoutBOMFile) {
    // This array represents a file with a UTF-16 with a BOM and the contents:
    // port: 1234
    // <blank line>
    std::vector<unsigned char> data{0x70, 0x00, 0x6f, 0x00, 0x72, 0x00, 0x74, 0x00,
                                    0x3a, 0x00, 0x20, 0x00, 0x31, 0x00, 0x32, 0x00,
                                    0x33, 0x00, 0x34, 0x00, 0x0d, 0x00, 0x0a, 0x00};
    TestFile(data, false);
}

// Positive: Validate a UTF-8 file with a BOM can be parsed
TEST(YAMLConfigFile, UTF8WithBOMFile) {
    // This array represents a file with a UTF-8 BOM and the contents:
    // port: 1234
    // <blank line>
    std::vector<unsigned char> data{
        0xef, 0xbb, 0xbf, 0x70, 0x6f, 0x72, 0x74, 0x3a, 0x20, 0x31, 0x32, 0x33, 0x34, 0x0d, 0x0a};
    TestFile(data, true);
}
#endif

}  // unnamed namespace
