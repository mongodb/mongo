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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/platform/basic.h"

#include "mongo/idl/config_option_no_init_test_gen.h"
#include "mongo/idl/config_option_test_gen.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/cmdline_utils/censor_cmdline_test.h"
#include "mongo/util/options_parser/options_parser.h"
#include "mongo/util/options_parser/startup_option_init.h"
#include "mongo/util/options_parser/startup_options.h"

namespace mongo {
namespace test {

namespace moe = ::mongo::optionenvironment;
bool gEnableTestConfigOpt14 = true;
bool gEnableTestConfigOpt15 = false;

namespace {

Status parseArgv(const std::vector<std::string>& argv, moe::Environment* parsed) {
    auto status = moe::OptionsParser().run(moe::startupOptions, argv, {}, parsed);
    if (!status.isOK()) {
        return status;
    }
    return parsed->validate();
}

Status parseConfig(const std::string& config, moe::Environment* parsed) {
    auto status = moe::OptionsParser().runConfigFile(moe::startupOptions, config, {}, parsed);
    if (!status.isOK()) {
        return status;
    }
    return parsed->validate();
}

Status parseMixed(const std::vector<std::string>& argv,
                  const std::string& config,
                  moe::Environment* env) try {
    moe::OptionsParser mixedParser;

    moe::Environment conf;
    uassertStatusOK(mixedParser.runConfigFile(moe::startupOptions, config, {}, &conf));
    uassertStatusOK(env->setAll(conf));

    moe::Environment cli;
    uassertStatusOK(mixedParser.run(moe::startupOptions, argv, {}, &cli));
    uassertStatusOK(env->setAll(cli));

    return env->validate();
} catch (const DBException& ex) {
    return ex.toStatus();
}

MONGO_STARTUP_OPTIONS_PARSE(ConfigOption)(InitializerContext*) {
    // Fake argv for default arg parsing.
    const std::vector<std::string> argv = {
        "mongo",
        "--testConfigOpt2",
        "true",
        "--testConfigOpt8",
        "8",
        "--testConfigOpt12",
        "command-line option",
        "--testConfigOpt14",
        "set14",
    };
    return parseArgv(argv, &moe::startupOptionsParsed);
}

template <typename T>
void ASSERT_OPTION_SET(const moe::Environment& env, const moe::Key& name, const T& exp) {
    ASSERT_TRUE(env.count(name));
    ASSERT_EQ(env[name].as<T>(), exp);
}

// ASSERT_EQ can't handle vectors, so take slightly more pains.
template <typename T>
void ASSERT_VECTOR_OPTION_SET(const moe::Environment& env,
                              const moe::Key& name,
                              const std::vector<T>& exp) {
    ASSERT_TRUE(env.count(name));
    auto value = env[name].as<std::vector<T>>();
    ASSERT_EQ(exp.size(), value.size());
    for (size_t i = 0; i < exp.size(); ++i) {
        ASSERT_EQ(exp[i], value[i]);
    }
}

template <typename T>
void ASSERT_OPTION_NOT_SET(const moe::Environment& env, const moe::Key& name) {
    ASSERT_FALSE(env.count(name));
    ASSERT_THROWS(env[name].as<T>(), AssertionException);
}

TEST(ConfigOption, Opt1) {
    ASSERT_OPTION_NOT_SET<bool>(moe::startupOptionsParsed, "test.config.opt1");

    moe::Environment parsed;
    ASSERT_OK(parseArgv({"mongod", "--testConfigOpt1"}, &parsed));
    ASSERT_OPTION_SET<bool>(parsed, "test.config.opt1", true);

    moe::Environment parsedYAML;
    ASSERT_OK(parseConfig("test: { config: { opt1: true } }", &parsedYAML));
    ASSERT_OPTION_SET<bool>(parsedYAML, "test.config.opt1", true);

    moe::Environment parsedINI;
    ASSERT_OK(parseConfig("testConfigOpt1=true", &parsedINI));
    ASSERT_OPTION_SET<bool>(parsedINI, "test.config.opt1", true);
}

TEST(ConfigOption, Opt2) {
    ASSERT_OPTION_SET<bool>(moe::startupOptionsParsed, "test.config.opt2", true);

    moe::Environment parsedAbsent;
    ASSERT_OK(parseArgv({"mongod"}, &parsedAbsent));
    ASSERT_OPTION_NOT_SET<bool>(parsedAbsent, "test.config.opt2");

    moe::Environment parsedTrue;
    ASSERT_OK(parseArgv({"mongod", "--testConfigOpt2", "true"}, &parsedTrue));
    ASSERT_OPTION_SET<bool>(parsedTrue, "test.config.opt2", true);

    moe::Environment parsedFalse;
    ASSERT_OK(parseArgv({"mongod", "--testConfigOpt2", "false"}, &parsedFalse));
    ASSERT_OPTION_SET<bool>(parsedFalse, "test.config.opt2", false);

    moe::Environment parsedFail;
    ASSERT_NOT_OK(parseArgv({"mongod", "--testConfigOpt2"}, &parsedFail));
    ASSERT_NOT_OK(parseArgv({"mongod", "--testConfigOpt2", "banana"}, &parsedFail));
    ASSERT_NOT_OK(parseConfig("test: { config: { opt2: true } }", &parsedFail));
    ASSERT_NOT_OK(parseConfig("testConfigOpt2=true", &parsedFail));
}

TEST(ConfigOption, Opt3) {
    ASSERT_OPTION_NOT_SET<bool>(moe::startupOptionsParsed, "test.config.opt3");

    moe::Environment parsedAbsent;
    ASSERT_OK(parseArgv({"mongod"}, &parsedAbsent));
    ASSERT_OPTION_NOT_SET<bool>(parsedAbsent, "test.config.opt3");

    moe::Environment parsedTrue;
    ASSERT_OK(parseArgv({"mongod", "--testConfigOpt3", "true"}, &parsedTrue));
    ASSERT_OPTION_SET<bool>(parsedTrue, "test.config.opt3", true);

    moe::Environment parsedFalse;
    ASSERT_OK(parseArgv({"mongod", "--testConfigOpt3", "false"}, &parsedFalse));
    ASSERT_OPTION_SET<bool>(parsedFalse, "test.config.opt3", false);

    moe::Environment parsedImplicit;
    ASSERT_OK(parseArgv({"mongod", "--testConfigOpt3"}, &parsedImplicit));
    ASSERT_OPTION_SET<bool>(parsedImplicit, "test.config.opt3", true);
}

TEST(ConfigOption, Opt4) {
    ASSERT_OPTION_SET<std::string>(moe::startupOptionsParsed, "test.config.opt4", "Default Value");

    moe::Environment parsedDefault;
    ASSERT_OK(parseArgv({"mongod"}, &parsedDefault));
    ASSERT_OPTION_SET<std::string>(parsedDefault, "test.config.opt4", "Default Value");

    moe::Environment parsedHello;
    ASSERT_OK(parseArgv({"mongod", "--testConfigOpt4", "Hello"}, &parsedHello));
    ASSERT_OPTION_SET<std::string>(parsedHello, "test.config.opt4", "Hello");

    moe::Environment parsedFail;
    ASSERT_NOT_OK(parseArgv({"mongod", "--testConfigOpt4"}, &parsedFail));
}

TEST(ConfigOption, Opt5) {
    ASSERT_OPTION_NOT_SET<int>(moe::startupOptionsParsed, "test.config.opt5");

    moe::Environment parsedFail;
    ASSERT_NOT_OK(parseArgv({"mongod", "--testConfigOpt5"}, &parsedFail));
    ASSERT_NOT_OK(parseArgv({"mongod", "--testConfigOpt5", "123"}, &parsedFail));
    ASSERT_NOT_OK(parseConfig("test: { config: { opt5: 123 } }", &parsedFail));

    moe::Environment parsedINI;
    ASSERT_OK(parseConfig("testConfigOpt5=123", &parsedINI));
    ASSERT_OPTION_SET<int>(parsedINI, "test.config.opt5", 123);
}

TEST(ConfigOption, Opt6) {
    ASSERT_OPTION_NOT_SET<std::string>(moe::startupOptionsParsed, "testConfigOpt6");

    moe::Environment parsed;
    ASSERT_OK(parseArgv({"mongod", "some value"}, &parsed));
    ASSERT_OPTION_SET<std::string>(parsed, "testConfigOpt6", "some value");

    moe::Environment parsedINI;
    ASSERT_OK(parseConfig("testConfigOpt6=other thing", &parsedINI));
    ASSERT_OPTION_SET<std::string>(parsedINI, "testConfigOpt6", "other thing");
}

TEST(ConfigOption, Opt7) {
    ASSERT_OPTION_NOT_SET<std::vector<std::string>>(moe::startupOptionsParsed, "testConfigOpt7");

    // Single arg goes to opt6 per positioning.
    moe::Environment parsedSingleArg;
    ASSERT_OK(parseArgv({"mongod", "value1"}, &parsedSingleArg));
    ASSERT_OPTION_SET<std::string>(parsedSingleArg, "testConfigOpt6", "value1");
    ASSERT_OPTION_NOT_SET<std::vector<std::string>>(parsedSingleArg, "testConfigOpt7");

    moe::Environment parsedMultiArg;
    ASSERT_OK(parseArgv({"mongod", "value1", "value2", "value3"}, &parsedMultiArg));
    ASSERT_OPTION_SET<std::string>(parsedMultiArg, "testConfigOpt6", "value1");

    // ASSERT macros can't deal with vector<string>, so break out the test manually.
    ASSERT_VECTOR_OPTION_SET<std::string>(parsedMultiArg, "testConfigOpt7", {"value2", "value3"});
}

TEST(ConfigOption, Opt8) {
    ASSERT_OPTION_SET<long>(moe::startupOptionsParsed, "test.config.opt8", 8);

    moe::Environment parsed;
    ASSERT_OK(parseArgv({"mongod", "--testConfigOpt8", "42"}, &parsed));
    ASSERT_OPTION_SET<long>(parsed, "test.config.opt8", 42);

    moe::Environment parsedDeprShort;
    ASSERT_OK(parseArgv({"mongod", "--testConfigOpt8a", "43"}, &parsedDeprShort));
    ASSERT_OPTION_SET<long>(parsedDeprShort, "test.config.opt8", 43);

    moe::Environment parsedDeprDotted;
    ASSERT_OK(parseConfig("test: { config: { opt8b: 44 } }", &parsedDeprDotted));
    ASSERT_OPTION_SET<long>(parsedDeprDotted, "test.config.opt8", 44);
}

TEST(ConfigOption, Opt9) {
    ASSERT_OPTION_NOT_SET<unsigned>(moe::startupOptionsParsed, "test.config.opt9");
    ASSERT_OPTION_NOT_SET<long>(moe::startupOptionsParsed, "test.config.opt9a");
    ASSERT_OPTION_NOT_SET<unsigned long long>(moe::startupOptionsParsed, "test.config.opt9b");

    moe::Environment parsedCLI;
    ASSERT_OK(
        parseArgv({"mongod", "--testConfigOpt9", "42", "--testConfigOpt9a", "43"}, &parsedCLI));
    ASSERT_OPTION_SET<unsigned>(parsedCLI, "test.config.opt9", 42);
    ASSERT_OPTION_SET<long>(parsedCLI, "test.config.opt9a", 43);
    ASSERT_OPTION_NOT_SET<unsigned long long>(parsedCLI, "test.config.opt9b");

    moe::Environment parsedINI;
    ASSERT_OK(parseConfig("testConfigOpt9=42\ntestConfigOpt9a=43", &parsedINI));
    ASSERT_OPTION_SET<unsigned>(parsedINI, "test.config.opt9", 42);
    ASSERT_OPTION_SET<long>(parsedINI, "test.config.opt9a", 43);
    ASSERT_OPTION_NOT_SET<unsigned long long>(parsedINI, "test.config.opt9b");

    moe::Environment parsedYAML;
    ASSERT_OK(parseConfig("test: { config: { opt9: 42, opt9a: 43 } }", &parsedYAML));
    ASSERT_OPTION_SET<unsigned>(parsedYAML, "test.config.opt9", 42);
    ASSERT_OPTION_SET<long>(parsedYAML, "test.config.opt9a", 43);
    ASSERT_OPTION_NOT_SET<unsigned long long>(parsedYAML, "test.config.opt9b");

    moe::Environment parsedMixed;
    ASSERT_OK(parseMixed(
        {"mongod", "--testConfigOpt9", "42"}, "test: { config: { opt9a: 43 } }", &parsedMixed));
    ASSERT_OPTION_SET<unsigned>(parsedMixed, "test.config.opt9", 42);
    ASSERT_OPTION_SET<long>(parsedMixed, "test.config.opt9a", 43);
    ASSERT_OPTION_NOT_SET<unsigned long long>(parsedMixed, "test.config.opt9b");

    moe::Environment parsedFail;
    ASSERT_NOT_OK(parseArgv({"mongod", "--testConfigOpt9", "42"}, &parsedFail));
    ASSERT_NOT_OK(
        parseArgv({"mongod", "--testConfigOpt9", "42", "--testConfigOpt9b", "44"}, &parsedFail));
    ASSERT_NOT_OK(parseArgv(
        {"mongod", "--testConfigOpt9", "42", "--testConfigOpt9a", "43", "--testConfigOpt9b", "44"},
        &parsedFail));
    ASSERT_NOT_OK(parseConfig("testConfigOpt9=42", &parsedFail));
    ASSERT_NOT_OK(parseConfig("testConfigOpt9=42\ntestConfigOpt9b=44", &parsedFail));
    ASSERT_NOT_OK(
        parseConfig("testConfigOpt9=42\ntestConfigOpt9a=43\ntestConfigOpt9b=44", &parsedFail));
    ASSERT_NOT_OK(parseConfig("test: { config: { opt9: 42 } }", &parsedFail));
    ASSERT_NOT_OK(parseConfig("test: { config: { opt9: 42, opt9b: 44 } }", &parsedFail));
    ASSERT_NOT_OK(parseConfig("test: { config: { opt9: 42, opt9a: 43, opt9b: 44 } }", &parsedFail));
}

TEST(ConfigOption, Opt10) {
    ASSERT_OPTION_NOT_SET<int>(moe::startupOptionsParsed, "test.config.opt10a");
    ASSERT_OPTION_NOT_SET<int>(moe::startupOptionsParsed, "test.config.opt10b");

    const auto tryParse = [](int a, int b) {
        moe::Environment parsed;
        ASSERT_OK(parseArgv({"mongod",
                             "--testConfigOpt10a",
                             std::to_string(a),
                             "--testConfigOpt10b",
                             std::to_string(b)},
                            &parsed));
        ASSERT_OPTION_SET<int>(parsed, "test.config.opt10a", a);
        ASSERT_OPTION_SET<int>(parsed, "test.config.opt10b", b);
    };
    const auto failParse = [](int a, int b) {
        moe::Environment parsedFail;
        ASSERT_NOT_OK(parseArgv({"mongod",
                                 "--testConfigOpt10a",
                                 std::to_string(a),
                                 "--testConfigOpt10b",
                                 std::to_string(b)},
                                &parsedFail));
    };
    tryParse(1, 1);
    tryParse(99, 99);
    tryParse(1, 0);
    tryParse(99, 100);
    failParse(0, 0);
    failParse(100, 100);
}

TEST(ConfigOption, Opt11) {
    ASSERT_OPTION_NOT_SET<int>(moe::startupOptionsParsed, "test.config.opt11");

    const auto tryParse = [](int val) {
        moe::Environment parsed;
        ASSERT_OK(parseArgv({"mongod", "--testConfigOpt11", std::to_string(val)}, &parsed));
        ASSERT_OPTION_SET<int>(parsed, "test.config.opt11", val);
    };
    const auto failParse = [](int val) {
        moe::Environment parsed;
        ASSERT_NOT_OK(parseArgv({"mongod", "--testConfigOpt11", std::to_string(val)}, &parsed));
    };
    tryParse(1);
    tryParse(123456789);
    failParse(0);
    failParse(2);
    failParse(123456780);
}

TEST(ConfigOption, Opt12) {
    ASSERT_OPTION_SET<std::string>(
        moe::startupOptionsParsed, "test.config.opt12", "command-line option");
    ASSERT_EQ(gTestConfigOpt12, "command-line option");
}

TEST(ConfigOption, Opt13) {
    ASSERT_OPTION_NOT_SET<std::string>(moe::startupOptionsParsed, "test.config.opt13");

    moe::Environment parsedSingle;
    ASSERT_OK(parseArgv({"mongod", "-o", "single"}, &parsedSingle));
    ASSERT_OPTION_SET<std::string>(parsedSingle, "test.config.opt13", "single");

    moe::Environment parsedShort;
    ASSERT_OK(parseArgv({"mongod", "--testConfigOpt13", "short"}, &parsedShort));
    ASSERT_OPTION_SET<std::string>(parsedShort, "test.config.opt13", "short");
}

TEST(ConfigOption, Opt14) {
    ASSERT_OPTION_SET<std::string>(moe::startupOptionsParsed, "test.config.opt14", "set14");
    ASSERT_EQ(gTestConfigOpt14, "set14");
}

TEST(ConfigOption, Opt15) {
    ASSERT_OPTION_NOT_SET<std::string>(moe::startupOptionsParsed, "test.config.opt15");

    // Fails because the option was never declared.
    moe::Environment parseFail;
    ASSERT_NOT_OK(parseArgv({"mongod", "--testConfigOpt15", "set15"}, &parseFail));

    // Variable is declared.
    ASSERT_EQ(gTestConfigOpt15, "");
}

TEST(RedactionArgvVector, NothingCensored) {
    const std::vector<std::string> argv({"first",
                                         "second",
                                         "testConfigOpt16=KEEP",
                                         "---testConfigOpt16=KEEP",
                                         "testConfigOpt16",
                                         "KEEP"});
    censoringArgv(argv, argv);
    censoringVector(argv, argv);
}

TEST(RedactionArgv, DoubleHyphen) {
    const std::vector<std::string> argv({"first",
                                         "second",
                                         "--testConfigOpt16=LOSEME",
                                         "--testConfigOpt16",
                                         "Really, lose me!",
                                         "--testConfigOpt16depr=LOSEME",
                                         "--testConfigOpt16depr",
                                         "Really, lose me!"});

    const std::vector<std::string> expected({"first",
                                             "second",
                                             "--testConfigOpt16=xxxxxx",
                                             "--testConfigOpt16",
                                             "xxxxxxxxxxxxxxxx",
                                             "--testConfigOpt16depr=xxxxxx",
                                             "--testConfigOpt16depr",
                                             "xxxxxxxxxxxxxxxx"});

    ASSERT_EQ(expected.size(), argv.size());
    censoringArgv(expected, argv);
}

TEST(RedactionArgv, SingleHyphen) {
    const std::vector<std::string> argv({"first",
                                         "second",
                                         "-testConfigOpt16=LOSEME",
                                         "-testConfigOpt16",
                                         "Really, lose me!",
                                         "-testConfigOpt16depr=LOSEME",
                                         "-testConfigOpt16depr",
                                         "Really, lose me!"});

    const std::vector<std::string> expected({"first",
                                             "second",
                                             "-testConfigOpt16=xxxxxx",
                                             "-testConfigOpt16",
                                             "xxxxxxxxxxxxxxxx",
                                             "-testConfigOpt16depr=xxxxxx",
                                             "-testConfigOpt16depr",
                                             "xxxxxxxxxxxxxxxx"});

    ASSERT_EQ(expected.size(), argv.size());
    censoringArgv(expected, argv);
}

TEST(RedactionVector, DoubleHyphen) {
    const std::vector<std::string> argv({"first",
                                         "second",
                                         "--testConfigOpt16=LOSEME",
                                         "--testConfigOpt16",
                                         "Really, lose me!",
                                         "--testConfigOpt16depr=LOSEME",
                                         "--testConfigOpt16depr",
                                         "Really, lose me!"});

    const std::vector<std::string> expected({"first",
                                             "second",
                                             "--testConfigOpt16=<password>",
                                             "--testConfigOpt16",
                                             "<password>",
                                             "--testConfigOpt16depr=<password>",
                                             "--testConfigOpt16depr",
                                             "<password>"});

    ASSERT_EQ(expected.size(), argv.size());
    censoringVector(expected, argv);
}

TEST(RedactionVector, SingleHyphen) {
    const std::vector<std::string> argv({"first",
                                         "second",
                                         "-testConfigOpt16=LOSEME",
                                         "-testConfigOpt16",
                                         "Really, lose me!",
                                         "-testConfigOpt16depr=LOSEME",
                                         "-testConfigOpt16depr",
                                         "Really, lose me!"});

    const std::vector<std::string> expected({"first",
                                             "second",
                                             "-testConfigOpt16=<password>",
                                             "-testConfigOpt16",
                                             "<password>",
                                             "-testConfigOpt16depr=<password>",
                                             "-testConfigOpt16depr",
                                             "<password>"});

    ASSERT_EQ(expected.size(), argv.size());
    censoringVector(expected, argv);
}

TEST(RedactionBSON, Strings) {
    BSONObj obj = BSON("firstarg"
                       << "not a password"
                       << "test.config.opt16"
                       << "this password should be censored"
                       << "test.config.opt16depr"
                       << "this password should be censored"
                       << "middlearg"
                       << "also not a password"
                       << "test.config.opt16depr2"
                       << "this password should also be censored"
                       << "lastarg" << false);

    BSONObj res = BSON("firstarg"
                       << "not a password"
                       << "test.config.opt16"
                       << "<password>"
                       << "test.config.opt16depr"
                       << "<password>"
                       << "middlearg"
                       << "also not a password"
                       << "test.config.opt16depr2"
                       << "<password>"
                       << "lastarg" << false);

    cmdline_utils::censorBSONObj(&obj);
    ASSERT_BSONOBJ_EQ(res, obj);
}

TEST(RedactionBSON, Arrays) {
    BSONObj obj = BSON("firstarg"
                       << "not a password"
                       << "test.config.opt16"
                       << BSON_ARRAY("first censored password"
                                     << "next censored password")
                       << "test.config.opt16depr"
                       << BSON_ARRAY("first censored password"
                                     << "next censored password")
                       << "middlearg"
                       << "also not a password"
                       << "test.config.opt16depr2"
                       << BSON_ARRAY("first censored password"
                                     << "next censored password")
                       << "lastarg" << false);

    BSONObj res = BSON("firstarg"
                       << "not a password"
                       << "test.config.opt16"
                       << BSON_ARRAY("<password>"
                                     << "<password>")
                       << "test.config.opt16depr"
                       << BSON_ARRAY("<password>"
                                     << "<password>")
                       << "middlearg"
                       << "also not a password"
                       << "test.config.opt16depr2"
                       << BSON_ARRAY("<password>"
                                     << "<password>")
                       << "lastarg" << false);

    cmdline_utils::censorBSONObj(&obj);
    ASSERT_BSONOBJ_EQ(res, obj);
}

TEST(RedactionBSON, SubObjects) {
    BSONObj obj = BSON("firstarg"
                       << "not a password"
                       << "test"
                       << BSON("config" << BSON("opt16" << BSON_ARRAY("first censored password"
                                                                      << "next censored password")
                                                        << "opt16"
                                                        << "should be censored too"
                                                        << "opt16depr"
                                                        << BSON_ARRAY("first censored password"
                                                                      << "next censored password")
                                                        << "opt16depr"
                                                        << "should be censored too"))
                       << "lastarg" << false);

    BSONObj res = BSON("firstarg"
                       << "not a password"
                       << "test"
                       << BSON("config" << BSON("opt16" << BSON_ARRAY("<password>"
                                                                      << "<password>")
                                                        << "opt16"
                                                        << "<password>"
                                                        << "opt16depr"
                                                        << BSON_ARRAY("<password>"
                                                                      << "<password>")
                                                        << "opt16depr"
                                                        << "<password>"))
                       << "lastarg" << false);

    cmdline_utils::censorBSONObj(&obj);
    ASSERT_BSONOBJ_EQ(res, obj);
}

TEST(ConfigOption, Opt17) {
    ASSERT_OPTION_SET<std::int32_t>(
        moe::startupOptionsParsed, "test.config.opt17", kTestConfigOpt17Default);

    moe::Environment implicitParse;
    ASSERT_OK(parseArgv({"mongod", "--testConfigOpt17"}, &implicitParse));
    ASSERT_OPTION_SET<std::int32_t>(implicitParse, "test.config.opt17", kTestConfigOpt17Implicit);

    moe::Environment negativeParse;
    ASSERT_NOT_OK(
        parseArgv({"mongod", "--testConfigOpt17", std::to_string(kTestConfigOpt17Minimum - 1)},
                  &negativeParse));
    ASSERT_NOT_OK(
        parseArgv({"mongod", "--testConfigOpt17", std::to_string(kTestConfigOpt17Maximum + 1)},
                  &negativeParse));

    moe::Environment okParse;
    ASSERT_OK(parseArgv({"mongod", "--testConfigOpt17", std::to_string(kTestConfigOpt17Minimum)},
                        &okParse));
    ASSERT_OPTION_SET<std::int32_t>(okParse, "test.config.opt17", kTestConfigOpt17Minimum);
}

TEST(ConfigOptionNoInit, Opt1) {
    moe::OptionSection options("Options");
    ASSERT_OK(addIDLTestConfigs(&options));

    const std::vector<std::string> argv({
        "mongod",
        "--testConfigNoInitOpt1",
        "Hello",
    });
    moe::Environment parsed;
    ASSERT_OK(moe::OptionsParser().run(options, argv, {}, &parsed));
    ASSERT_OK(parsed.validate());
    ASSERT_OK(storeIDLTestConfigs(parsed));

    ASSERT_OPTION_SET<std::string>(parsed, "test.config.noInit.opt1", "Hello");
    ASSERT_EQ(gTestConfigNoInitOpt1, "Hello");
}

}  // namespace
}  // namespace test
}  // namespace mongo
