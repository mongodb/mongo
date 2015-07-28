/*    Copyright 2013 10gen Inc.
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
        ASSERT_OK(writerUse.rotate(true, logFileNameRotated));
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
