/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/replay/options_handler.h"

#include "mongo/unittest/unittest.h"

#include <fstream>
#include <string>
#include <vector>

namespace mongo {

class OptionsHandlerTest : public unittest::Test {

protected:
    std::string tempFilename1;
    std::string tempFilename2;
    std::string tempFileNameConfig;

    void setUp() override {
        // Generate a unique temporary filename
        tempFilename1 = "tmp_recording_file1.dat";
        tempFilename2 = "tmp_recording_file2.dat";
        tempFileNameConfig = "tmp_config_file.txt";
        // Create the temporary file and write fake data into it
        std::ofstream tempRecordingFile1(tempFilename1);
        std::ofstream tempRecordingFile2(tempFilename2);
        std::ofstream tempConfigFile(tempFileNameConfig);
        ASSERT_TRUE(boost::filesystem::exists(tempFilename1));
        ASSERT_TRUE(boost::filesystem::exists(tempFilename2));
        ASSERT_TRUE(tempRecordingFile1.is_open());
        ASSERT_TRUE(tempRecordingFile2.is_open());
        ASSERT_TRUE(tempConfigFile.is_open());
        tempRecordingFile1 << "test\n";
        tempRecordingFile2 << "test\n";
        tempConfigFile << "recordings:\n"
                       << "  - path: \"tmp_recording_file1.dat\"\n"
                       << "    uri: \"$local:12345\"\n"
                       << "  - path: \"tmp_recording_file2.dat\"\n"
                       << "    uri: \"$local:12346\"\n";
    }

    void tearDown() override {
        std::remove(tempFilename1.c_str());
        std::remove(tempFilename2.c_str());
        std::remove(tempFileNameConfig.c_str());
    }

    std::vector<char*> toArgv(const std::vector<std::string>& args) const {
        std::vector<char*> argv;
        for (const auto& arg : args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        return argv;
    }
};

TEST_F(OptionsHandlerTest, SimpleParseSingleInstance) {
    ASSERT_TRUE(boost::filesystem::exists(tempFilename1));
    std::vector<std::string> commandLine = {
        "fakeMongoR", "-i", tempFilename1, "-t", "$local:12345"};
    auto argv = toArgv(commandLine);
    OptionsHandler commandLineOptions;
    auto options = commandLineOptions.handle(argv.size(), argv.data());
    ASSERT_TRUE(options.size() == 1);
    auto& option = options[0];
    ASSERT_TRUE(option.recordingPath == "tmp_recording_file1.dat");
    ASSERT_TRUE(option.mongoURI == "$local:12345");
}

TEST_F(OptionsHandlerTest, SimpleParseMultipleInstances) {
    ASSERT_TRUE(boost::filesystem::exists(tempFileNameConfig));
    std::vector<std::string> commandLine = {"fakeMongoR", "-c", tempFileNameConfig};
    auto argv = toArgv(commandLine);
    OptionsHandler commandLineOptions;
    auto options = commandLineOptions.handle(argv.size(), argv.data());
    ASSERT_TRUE(options.size() == 2);
    auto& option1 = options[0];
    auto& option2 = options[1];
    ASSERT_TRUE(option1.recordingPath == "tmp_recording_file1.dat");
    ASSERT_TRUE(option1.mongoURI == "$local:12345");
    ASSERT_TRUE(option2.recordingPath == "tmp_recording_file2.dat");
    ASSERT_TRUE(option2.mongoURI == "$local:12346");
}

TEST_F(OptionsHandlerTest, IllFormedCommandLine) {
    OptionsHandler commandLineOptions;

    std::vector<std::string> commandLine = {"fakeMongoR", "-c", ""};
    auto argv = toArgv(commandLine);
    ASSERT_THROWS_CODE(commandLineOptions.handle(argv.size(), argv.data()),
                       DBException,
                       ErrorCodes::ReplayClientConfigurationError);

    std::vector<std::string> commandLine1 = {"fakeMongoR", "-i", "ffff"};
    auto argv1 = toArgv(commandLine1);
    ASSERT_THROWS_CODE(commandLineOptions.handle(argv1.size(), argv1.data()),
                       DBException,
                       ErrorCodes::ReplayClientConfigurationError);

    std::vector<std::string> commandLine2 = {"fakeMongoR", "-i", tempFilename1, "-t", ""};
    auto argv2 = toArgv(commandLine2);
    ASSERT_THROWS_CODE(commandLineOptions.handle(argv2.size(), argv2.data()),
                       DBException,
                       ErrorCodes::ReplayClientConfigurationError);
}

TEST_F(OptionsHandlerTest, ConfigFileIncompatibleWithSingleInstanceCmdLineArgs) {
    OptionsHandler commandLineOptions;
    std::vector<std::string> commandLine = {
        "fakeMongoR", "-c", tempFileNameConfig, "-i", tempFilename1, "-t", "$local:12345"};
    auto argv = toArgv(commandLine);
    ASSERT_THROWS_CODE(commandLineOptions.handle(argv.size(), argv.data()),
                       DBException,
                       ErrorCodes::ReplayClientConfigurationError);
}

}  // namespace mongo
