/**
 * Copyright (C) 2015 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include <boost/filesystem.hpp>

#include "mongo/base/init.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/ftdc/config.h"
#include "mongo/db/ftdc/file_reader.h"
#include "mongo/db/ftdc/file_writer.h"
#include "mongo/db/ftdc/ftdc_test.h"
#include "mongo/db/jsobj.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

const char* kTestFile = "metrics.test";

// File Sanity check
TEST(FTDCFileTest, TestFileBasicMetadata) {
    unittest::TempDir tempdir("metrics_testpath");
    boost::filesystem::path p(tempdir.path());
    p /= kTestFile;

    deleteFileIfNeeded(p);

    BSONObj doc1 = BSON("name"
                        << "joe"
                        << "key1"
                        << 34
                        << "key2"
                        << 45);
    BSONObj doc2 = BSON("name"
                        << "joe"
                        << "key3"
                        << 34
                        << "key5"
                        << 45);

    FTDCConfig config;
    FTDCFileWriter writer(&config);

    ASSERT_OK(writer.open(p));

    ASSERT_OK(writer.writeMetadata(doc1, Date_t()));
    ASSERT_OK(writer.writeMetadata(doc2, Date_t()));

    writer.close();

    FTDCFileReader reader;
    ASSERT_OK(reader.open(p));

    ASSERT_OK(reader.hasNext());

    BSONObj doc1a = std::get<1>(reader.next());

    ASSERT_TRUE(doc1 == doc1a);

    ASSERT_OK(reader.hasNext());

    BSONObj doc2a = std::get<1>(reader.next());

    ASSERT_TRUE(doc2 == doc2a);

    auto sw = reader.hasNext();
    ASSERT_OK(sw);
    ASSERT_EQUALS(sw.getValue(), false);
}

// File Sanity check
TEST(FTDCFileTest, TestFileBasicCompress) {
    unittest::TempDir tempdir("metrics_testpath");
    boost::filesystem::path p(tempdir.path());
    p /= kTestFile;

    deleteFileIfNeeded(p);

    BSONObj doc1 = BSON("name"
                        << "joe"
                        << "key1"
                        << 34
                        << "key2"
                        << 45);
    BSONObj doc2 = BSON("name"
                        << "joe"
                        << "key3"
                        << 34
                        << "key5"
                        << 45);

    FTDCConfig config;
    FTDCFileWriter writer(&config);

    ASSERT_OK(writer.open(p));

    ASSERT_OK(writer.writeSample(doc1, Date_t()));
    ASSERT_OK(writer.writeSample(doc2, Date_t()));

    writer.close();

    FTDCFileReader reader;
    ASSERT_OK(reader.open(p));

    ASSERT_OK(reader.hasNext());

    BSONObj doc1a = std::get<1>(reader.next());

    ASSERT_TRUE(doc1 == doc1a);

    ASSERT_OK(reader.hasNext());

    BSONObj doc2a = std::get<1>(reader.next());

    ASSERT_TRUE(doc2 == doc2a);

    auto sw = reader.hasNext();
    ASSERT_OK(sw);
    ASSERT_EQUALS(sw.getValue(), false);
}

/**
 * Validates all the data that gets written to file is returned as is
 */
class FileTestTie {
public:
    FileTestTie()
        : _tempdir("metrics_testpath"),
          _path(boost::filesystem::path(_tempdir.path()) / kTestFile),
          _writer(&_config) {
        deleteFileIfNeeded(_path);

        ASSERT_OK(_writer.open(_path));
    }

    ~FileTestTie() {
        validate();
    }

    void addSample(const BSONObj& sample) {
        ASSERT_OK(_writer.writeSample(sample, Date_t()));
        _docs.emplace_back(sample);
    }

private:
    void validate(bool forceCompress = true) {
        _writer.close();

        ValidateDocumentList(_path, _docs);
    }

private:
    unittest::TempDir _tempdir;
    boost::filesystem::path _path;
    FTDCConfig _config;
    FTDCFileWriter _writer;
    std::vector<BSONObj> _docs;
};

// Test various schema changes
TEST(FTDCFileTest, TestSchemaChanges) {
    FileTestTie c;

    c.addSample(BSON("name"
                     << "joe"
                     << "key1"
                     << 33
                     << "key2"
                     << 42));
    c.addSample(BSON("name"
                     << "joe"
                     << "key1"
                     << 34
                     << "key2"
                     << 45));
    c.addSample(BSON("name"
                     << "joe"
                     << "key1"
                     << 34
                     << "key2"
                     << 45));

    // Add Value
    c.addSample(BSON("name"
                     << "joe"
                     << "key1"
                     << 34
                     << "key2"
                     << 45
                     << "key3"
                     << 47));

    c.addSample(BSON("name"
                     << "joe"
                     << "key1"
                     << 34
                     << "key2"
                     << 45
                     << "key3"
                     << 47));

    // Rename field
    c.addSample(BSON("name"
                     << "joe"
                     << "key1"
                     << 34
                     << "key5"
                     << 45
                     << "key3"
                     << 47));

    // Change type
    c.addSample(BSON("name"
                     << "joe"
                     << "key1"
                     << 34
                     << "key5"
                     << "45"
                     << "key3"
                     << 47));

    // RemoveField
    c.addSample(BSON("name"
                     << "joe"
                     << "key5"
                     << "45"
                     << "key3"
                     << 47));
}

// Test a full buffer
TEST(FTDCFileTest, TestFull) {
    // Test a large numbers of zeros, and incremental numbers in a full buffer
    for (int j = 0; j < 2; j++) {
        FileTestTie c;

        c.addSample(BSON("name"
                         << "joe"
                         << "key1"
                         << 33
                         << "key2"
                         << 42));

        for (size_t i = 0; i <= FTDCConfig::kMaxSamplesPerArchiveMetricChunkDefault - 2; i++) {
            c.addSample(BSON("name"
                             << "joe"
                             << "key1"
                             << static_cast<long long int>(i * j)
                             << "key2"
                             << 45));
        }

        c.addSample(BSON("name"
                         << "joe"
                         << "key1"
                         << 34
                         << "key2"
                         << 45));

        // Add Value
        c.addSample(BSON("name"
                         << "joe"
                         << "key1"
                         << 34
                         << "key2"
                         << 45));
    }
}

// Test a bad file
TEST(FTDCFileTest, TestBadFile) {
    unittest::TempDir tempdir("metrics_testpath");
    boost::filesystem::path p(tempdir.path());
    p /= kTestFile;

    std::ofstream stream(p.c_str());
    // This test case caused us to allocate more memory then the size of the file the first time I
    // tried it
    stream << "Hello World";

    stream.close();

    FTDCFileReader reader;
    ASSERT_OK(reader.open(p));

    auto sw = reader.hasNext();
    ASSERT_NOT_OK(sw);
}

}  // namespace mongo
