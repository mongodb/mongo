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

#include "mongo/db/ftdc/file_writer.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/ftdc/config.h"
#include "mongo/db/ftdc/file_reader.h"
#include "mongo/db/ftdc/ftdc_test.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"

#include <cmath>
#include <tuple>
#include <vector>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

namespace mongo {

const char* kTestFile = "metrics.test";
const char* kTestFileCopy = "metrics.test.copy";

class FTDCFileTest : public ServiceContextTest {};

// File Sanity check
TEST_F(FTDCFileTest, TestFileBasicMetadata) {
    unittest::TempDir tempdir("metrics_testpath");
    boost::filesystem::path p(tempdir.path());
    p /= kTestFile;

    deleteFileIfNeeded(p);

    BSONObj doc1 = BSON("name" << "joe"
                               << "key1" << 34 << "key2" << 45);
    BSONObj doc2 = BSON("name" << "joe"
                               << "key3" << 34 << "key5" << 45);

    FTDCConfig config;
    FTDCFileWriter writer(&config);

    ASSERT_OK(writer.open(p));

    ASSERT_OK(writer.writeMetadata(doc1, Date_t()));
    ASSERT_OK(writer.writeMetadata(doc2, Date_t()));

    writer.close().transitional_ignore();

    FTDCFileReader reader;
    ASSERT_OK(reader.open(p));

    ASSERT_OK(reader.hasNext());

    BSONObj doc1a = std::get<1>(reader.next());

    ASSERT_BSONOBJ_EQ(doc1, doc1a);

    ASSERT_OK(reader.hasNext());

    BSONObj doc2a = std::get<1>(reader.next());

    ASSERT_BSONOBJ_EQ(doc2, doc2a);

    auto sw = reader.hasNext();
    ASSERT_OK(sw);
    ASSERT_EQUALS(sw.getValue(), false);
}

// File Sanity check
TEST_F(FTDCFileTest, TestFileBasicCompress) {
    unittest::TempDir tempdir("metrics_testpath");
    boost::filesystem::path p(tempdir.path());
    p /= kTestFile;

    deleteFileIfNeeded(p);

    BSONObj doc1 = BSON("name" << "joe"
                               << "key1" << 34 << "key2" << 45);
    BSONObj doc2 = BSON("name" << "joe"
                               << "key3" << 34 << "key5" << 45);

    FTDCConfig config;
    FTDCFileWriter writer(&config);

    ASSERT_OK(writer.open(p));

    ASSERT_OK(writer.writeSample(doc1, Date_t()));
    ASSERT_OK(writer.writeSample(doc2, Date_t()));

    writer.close().transitional_ignore();

    FTDCFileReader reader;
    ASSERT_OK(reader.open(p));

    ASSERT_OK(reader.hasNext());

    BSONObj doc1a = std::get<1>(reader.next());

    ASSERT_BSONOBJ_EQ(doc1, doc1a);

    ASSERT_OK(reader.hasNext());

    BSONObj doc2a = std::get<1>(reader.next());

    ASSERT_BSONOBJ_EQ(doc2, doc2a);

    auto sw = reader.hasNext();
    ASSERT_OK(sw);
    ASSERT_EQUALS(sw.getValue(), false);
}

TEST_F(FTDCFileTest, TestFileBasicPeriodicMetadata) {
    unittest::TempDir tempdir("metrics_testpath");
    boost::filesystem::path p(tempdir.path());
    p /= kTestFile;

    deleteFileIfNeeded(p);

    BSONObj subObj1 = BSON("f1_1" << 101 << "f1_2" << 102);
    BSONObj subObj2 = BSON("f2_1" << 201 << "f2_2" << 202);
    BSONObj altSubObj1 = BSON("f1_1" << 1001 << "f1_2" << 1002);
    BSONObj altSubObj2 = BSON("f2_1" << 2001 << "f2_2" << 2002);

    BSONObj doc1 = BSON("field1" << subObj1 << "field2" << subObj2);
    BSONObj doc2 = BSON("field1" << altSubObj1 << "field2" << subObj2);
    BSONObj doc3 = BSON("field1" << altSubObj1 << "field2" << altSubObj2);
    BSONObj doc4 = BSON("field1" << subObj2 << "field2" << subObj1);
    BSONObj deltaDoc1 = BSON("field1" << altSubObj1);
    BSONObj deltaDoc2 = BSON("field2" << altSubObj2);

    FTDCConfig config;
    FTDCFileWriter writer(&config);

    ASSERT_OK(writer.open(p));
    ASSERT_OK(writer.writePeriodicMetadataSample(doc1, Date_t()));  // writes doc1, resets delta
    for (int i = 0; i < 3; i++) {
        ASSERT_OK(writer.writePeriodicMetadataSample(doc2, Date_t()));  // writes deltaDoc1 once
    }
    ASSERT_OK(writer.writePeriodicMetadataSample(doc3, Date_t()));  // writes deltaDoc2
    writer.close().transitional_ignore();
    ASSERT_OK(writer.open(p));                                      // resets delta
    ASSERT_OK(writer.writePeriodicMetadataSample(doc2, Date_t()));  // writes doc2, resets delta
    ASSERT_OK(writer.writePeriodicMetadataSample(doc3, Date_t()));  // writes deltaDoc2
    ASSERT_OK(writer.writePeriodicMetadataSample(doc4, Date_t()));  // writes doc4, resets delta

    writer.close().transitional_ignore();

    FTDCFileReader reader;
    ASSERT_OK(reader.open(p));

    ASSERT_OK(reader.hasNext());
    BSONObj doc1a = std::get<1>(reader.next());
    ASSERT_BSONOBJ_EQ(doc1, doc1a);

    ASSERT_OK(reader.hasNext());
    BSONObj doc2a = std::get<1>(reader.next());
    ASSERT_BSONOBJ_EQ(deltaDoc1, doc2a);

    ASSERT_OK(reader.hasNext());
    BSONObj doc3a = std::get<1>(reader.next());
    ASSERT_BSONOBJ_EQ(deltaDoc2, doc3a);

    ASSERT_OK(reader.hasNext());
    BSONObj doc4a = std::get<1>(reader.next());
    ASSERT_BSONOBJ_EQ(doc2, doc4a);

    ASSERT_OK(reader.hasNext());
    BSONObj doc5a = std::get<1>(reader.next());
    ASSERT_BSONOBJ_EQ(deltaDoc2, doc5a);

    ASSERT_OK(reader.hasNext());
    BSONObj doc6a = std::get<1>(reader.next());
    ASSERT_BSONOBJ_EQ(doc4, doc6a);

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
    void validate() {
        // Verify we are flushing writes correctly by copying the file, and then reading it.
        auto tempfile(boost::filesystem::path(_tempdir.path()) / kTestFileCopy);
        boost::filesystem::copy_file(_path, tempfile);

        // Read the file to make sure it is correct.
        // We do not verify contents because the compressor may not have flushed the final records
        // which is expected.
        {
            FTDCFileReader reader;

            ASSERT_OK(reader.open(tempfile));

            auto sw = reader.hasNext();
            while (sw.isOK() && sw.getValue()) {
                sw = reader.hasNext();
            }

            ASSERT_OK(sw);
        }

        _writer.close().transitional_ignore();

        ValidateDocumentList(_path, _docs, FTDCValidationMode::kStrict);
    }

private:
    unittest::TempDir _tempdir;
    boost::filesystem::path _path;
    FTDCConfig _config;
    FTDCFileWriter _writer;
    std::vector<BSONObj> _docs;
};

// Test various schema changes
TEST_F(FTDCFileTest, TestSchemaChanges) {
    FileTestTie c;

    c.addSample(BSON("name" << "joe"
                            << "key1" << 33 << "key2" << 42));
    c.addSample(BSON("name" << "joe"
                            << "key1" << 34 << "key2" << 45));
    c.addSample(BSON("name" << "joe"
                            << "key1" << 34 << "key2" << 45));

    // Add Value
    c.addSample(BSON("name" << "joe"
                            << "key1" << 34 << "key2" << 45 << "key3" << 47));

    c.addSample(BSON("name" << "joe"
                            << "key1" << 34 << "key2" << 45 << "key3" << 47));

    // Rename field
    c.addSample(BSON("name" << "joe"
                            << "key1" << 34 << "key5" << 45 << "key3" << 47));

    // Change type
    c.addSample(BSON("name" << "joe"
                            << "key1" << 34 << "key5"
                            << "45"
                            << "key3" << 47));

    // RemoveField
    c.addSample(BSON("name" << "joe"
                            << "key5"
                            << "45"
                            << "key3" << 47));
}

// Test a full buffer
TEST_F(FTDCFileTest, TestFull) {
    // Test a large numbers of zeros, and incremental numbers in a full buffer
    for (int j = 0; j < 2; j++) {
        FileTestTie c;

        c.addSample(BSON("name" << "joe"
                                << "key1" << 33 << "key2" << 42));

        for (size_t i = 0; i <= FTDCConfig::kMaxSamplesPerArchiveMetricChunkDefault - 2; i++) {
            c.addSample(BSON("name" << "joe"
                                    << "key1" << static_cast<long long int>(i * j) << "key2"
                                    << 45));
        }

        c.addSample(BSON("name" << "joe"
                                << "key1" << 34 << "key2" << 45));

        // Add Value
        c.addSample(BSON("name" << "joe"
                                << "key1" << 34 << "key2" << 45));
    }
}

// Test a large documents so that we cause multiple 4kb buffers to flush on Windows.
TEST_F(FTDCFileTest, TestLargeDocuments) {
    FileTestTie c;

    for (int j = 0; j < 5; j++) {
        for (size_t i = 0; i <= FTDCConfig::kMaxSamplesPerArchiveMetricChunkDefault; i++) {
            BSONObjBuilder b;
            b.append("name", "joe");
            for (size_t k = 0; k < 200; k++) {
                b.appendNumber(
                    "num",
                    static_cast<long long int>(i * j + 5000 - (sin(static_cast<double>(k)) * 100)));
            }

            c.addSample(b.obj());
        }
    }
}

// Test a bad file
TEST_F(FTDCFileTest, TestBadFile) {
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
