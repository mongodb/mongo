/*    Copyright 2013 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "mongo/platform/basic.h"

#include <fstream>

#include "mongo/logger/rotatable_file_writer.h"
#include "mongo/unittest/unittest.h"

namespace {
    using namespace mongo;
    using namespace mongo::logger;

    const std::string logFileName("LogTest_RotatableFileAppender.txt");
    const std::string logFileNameRotated("LogTest_RotatableFileAppender_Rotated.txt");

    // TODO(schwerin): Create a safe, uniform mechanism by which unit tests may read and write
    // temporary files.
    class RotatableFileWriterTest : public mongo::unittest::Test {
    public:
        RotatableFileWriterTest() {
            unlink(logFileName.c_str());
            unlink(logFileNameRotated.c_str());
        }

        virtual ~RotatableFileWriterTest() {
            unlink(logFileName.c_str());
            unlink(logFileNameRotated.c_str());
        }
    };

    TEST_F(RotatableFileWriterTest, RotationTest) {
        using namespace logger;

        {
            RotatableFileWriter writer;
            RotatableFileWriter::Use writerUse(&writer);
            ASSERT_OK(writerUse.setFileName(logFileName, false));
            ASSERT_TRUE(writerUse.stream() << "Level 1 message." << std::endl);
            ASSERT_TRUE(writerUse.stream() << "Level 2 message." << std::endl);
            ASSERT_OK(writerUse.rotate(logFileNameRotated));
            ASSERT_TRUE(writerUse.stream() << "Level 3 message." << std::endl);
            ASSERT_TRUE(writerUse.stream() << "Level 4 message." << std::endl);
        }

        {
            std::ifstream ifs(logFileNameRotated.c_str());
            ASSERT_TRUE(ifs.is_open());
            ASSERT_TRUE(ifs.good());
            std::string input;
            ASSERT_TRUE(std::getline(ifs, input));
            ASSERT_EQUALS(input, "Level 1 message.");
            ASSERT_TRUE(std::getline(ifs, input));
            ASSERT_EQUALS(input, "Level 2 message.");
            ASSERT_TRUE(std::getline(ifs, input).fail());
            ASSERT_TRUE(ifs.eof());
        }

        {
            std::ifstream ifs(logFileName.c_str());
            ASSERT_TRUE(ifs.is_open());
            ASSERT_TRUE(ifs.good());
            std::string input;
            ASSERT_TRUE(std::getline(ifs, input));
            ASSERT_EQUALS(input, "Level 3 message.");
            ASSERT_TRUE(std::getline(ifs, input));
            ASSERT_EQUALS(input, "Level 4 message.");
            ASSERT_TRUE(std::getline(ifs, input).fail());
            ASSERT_TRUE(ifs.eof());
        }

        {
            RotatableFileWriter writer;
            RotatableFileWriter::Use writerUse(&writer);
            ASSERT_OK(writerUse.setFileName(logFileName, true));
            ASSERT_TRUE(writerUse.stream() << "Level 5 message." << std::endl);
            ASSERT_TRUE(writerUse.stream() << "Level 6 message." << std::endl);
        }

       {
            std::ifstream ifs(logFileName.c_str());
            ASSERT_TRUE(ifs.is_open());
            ASSERT_TRUE(ifs.good());
            std::string input;
            ASSERT_TRUE(std::getline(ifs, input));
            ASSERT_EQUALS(input, "Level 3 message.");
            ASSERT_TRUE(std::getline(ifs, input));
            ASSERT_EQUALS(input, "Level 4 message.");
            ASSERT_FALSE(std::getline(ifs, input).fail());
            ASSERT_EQUALS(input, "Level 5 message.");
            ASSERT_TRUE(std::getline(ifs, input));
            ASSERT_EQUALS(input, "Level 6 message.");
            ASSERT_TRUE(std::getline(ifs, input).fail());
            ASSERT_TRUE(ifs.eof());
        }

        {
            RotatableFileWriter writer;
            RotatableFileWriter::Use writerUse(&writer);
            ASSERT_OK(writerUse.setFileName(logFileName, false));
            ASSERT_TRUE(writerUse.stream() << "Level 7 message." << std::endl);
            ASSERT_TRUE(writerUse.stream() << "Level 8 message." << std::endl);
        }

        {
            std::ifstream ifs(logFileName.c_str());
            ASSERT_TRUE(ifs.is_open());
            ASSERT_TRUE(ifs.good());
            std::string input;
            ASSERT_TRUE(std::getline(ifs, input));
            ASSERT_EQUALS(input, "Level 7 message.");
            ASSERT_FALSE(std::getline(ifs, input).fail());
            ASSERT_EQUALS(input, "Level 8 message.");
            ASSERT_TRUE(std::getline(ifs, input).fail());
            ASSERT_TRUE(ifs.eof());
        }
    }

}  // namespace mongo
