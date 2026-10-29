// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/replay/config_handler.h"

#include "mongo/unittest/unittest.h"

#include <fstream>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>

namespace mongo {

class ConfigHandlerTest : public unittest::Test {

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
        tempConfigFile << "{"
                       << "\"recordings\": ["
                       << "{"
                       << "\"path\": \"tmp_recording_file1.dat\","
                       << "\"uri\": \"$local:12345\""
                       << "},"
                       << "{"
                       << "\"path\": \"tmp_recording_file2.dat\","
                       << "\"uri\": \"$local:12346\""
                       << "}"
                       << "]"
                       << "}";
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

TEST_F(ConfigHandlerTest, SimpleParseSingleInstancePerfRecordingDisabled) {
    ASSERT_TRUE(boost::filesystem::exists(tempFilename1));
    std::vector<std::string> commandLine = {
        "fakeMongoR", "-i", tempFilename1, "-t", "$local:12345"};
    auto argv = toArgv(commandLine);
    ConfigHandler configHandler;
    auto options = configHandler.parse(argv.size(), argv.data());
    ASSERT_TRUE(options.size() == 1);
    auto& option = options[0];
    ASSERT_TRUE(option.recordingPath == "tmp_recording_file1.dat");
    ASSERT_TRUE(option.mongoURI == "$local:12345");
    ASSERT_TRUE(option.enablePerformanceRecording.empty());
}

TEST_F(ConfigHandlerTest, SimpleParseSingleInstancePerfRecordingEnabled) {
    ASSERT_TRUE(boost::filesystem::exists(tempFilename1));
    std::vector<std::string> commandLine = {
        "fakeMongoR", "-i", tempFilename1, "-t", "$local:12345", "-p", "test.bin"};
    auto argv = toArgv(commandLine);
    ConfigHandler configHandler;
    auto options = configHandler.parse(argv.size(), argv.data());
    ASSERT_TRUE(options.size() == 1);
    auto& option = options[0];
    ASSERT_TRUE(option.recordingPath == "tmp_recording_file1.dat");
    ASSERT_TRUE(option.mongoURI == "$local:12345");
    ASSERT_EQ(option.enablePerformanceRecording, "test.bin");
}

TEST_F(ConfigHandlerTest, SimpleParseMultipleInstances) {
    ASSERT_TRUE(boost::filesystem::exists(tempFileNameConfig));
    std::vector<std::string> commandLine = {"fakeMongoR", "-c", tempFileNameConfig};
    auto argv = toArgv(commandLine);
    ConfigHandler configHandler;
    auto options = configHandler.parse(argv.size(), argv.data());
    ASSERT_TRUE(options.size() == 2);
    auto& option1 = options[0];
    auto& option2 = options[1];
    ASSERT_TRUE(option1.recordingPath == "tmp_recording_file1.dat");
    ASSERT_TRUE(option1.mongoURI == "$local:12345");
    ASSERT_TRUE(option2.recordingPath == "tmp_recording_file2.dat");
    ASSERT_TRUE(option2.mongoURI == "$local:12346");
}

TEST_F(ConfigHandlerTest, IllFormedCommandLine) {
    ConfigHandler configHandler;

    std::vector<std::string> commandLine = {"fakeMongoR", "-c", ""};
    auto argv = toArgv(commandLine);
    ASSERT_THROWS_CODE(configHandler.parse(argv.size(), argv.data()),
                       DBException,
                       ErrorCodes::ReplayClientConfigurationError);

    std::vector<std::string> commandLine1 = {"fakeMongoR", "-i", "ffff"};
    auto argv1 = toArgv(commandLine1);
    ASSERT_THROWS_CODE(configHandler.parse(argv1.size(), argv1.data()),
                       DBException,
                       ErrorCodes::ReplayClientConfigurationError);

    std::vector<std::string> commandLine2 = {"fakeMongoR", "-i", tempFilename1, "-t", ""};
    auto argv2 = toArgv(commandLine2);
    ASSERT_THROWS_CODE(configHandler.parse(argv2.size(), argv2.data()),
                       DBException,
                       ErrorCodes::ReplayClientConfigurationError);
}

TEST_F(ConfigHandlerTest, ConfigFileIncompatibleWithSingleInstanceCmdLineArgs) {
    ConfigHandler configHandler;
    std::vector<std::string> commandLine = {
        "fakeMongoR", "-c", tempFileNameConfig, "-i", tempFilename1, "-t", "$local:12345"};
    auto argv = toArgv(commandLine);
    ASSERT_THROWS_CODE(configHandler.parse(argv.size(), argv.data()),
                       DBException,
                       ErrorCodes::ReplayClientConfigurationError);
}

}  // namespace mongo
