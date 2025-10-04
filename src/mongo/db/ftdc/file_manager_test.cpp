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


#include "mongo/db/ftdc/file_manager.h"

#include "mongo/base/data_type_endian.h"
#include "mongo/base/data_view.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/ftdc/collector.h"
#include "mongo/db/ftdc/config.h"
#include "mongo/db/ftdc/file_reader.h"
#include "mongo/db/ftdc/file_writer.h"
#include "mongo/db/ftdc/ftdc_test.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {

class FTDCFileManagerTest : public ServiceContextTest {
protected:
    void testPeriodicCollection();
};

// Test a full buffer
TEST_F(FTDCFileManagerTest, TestFull) {
    Client* client = &cc();
    FTDCConfig c;
    c.maxFileSizeBytes = 300;
    c.maxDirectorySizeBytes = 1000;
    c.maxSamplesPerInterimMetricChunk = 1;

    unittest::TempDir tempdir("metrics_testpath");
    boost::filesystem::path dir(tempdir.path());
    createDirectoryClean(dir);

    SyncFTDCCollectorCollection rotate;
    auto swMgr = FTDCFileManager::create(&c, dir, &rotate, client);
    ASSERT_OK(swMgr.getStatus());
    auto mgr = std::move(swMgr.getValue());

    // Test a large numbers of zeros, and incremental numbers in a full buffer
    for (int j = 0; j < 10; j++) {
        ASSERT_OK(mgr->writeSampleAndRotateIfNeeded(client,
                                                    BSON("name" << "joe"
                                                                << "key1" << 3230792343LL << "key2"
                                                                << 235135),
                                                    Date_t()));

        for (size_t i = 0; i <= FTDCConfig::kMaxSamplesPerArchiveMetricChunkDefault - 2; i++) {
            ASSERT_OK(mgr->writeSampleAndRotateIfNeeded(
                client,
                BSON("name" << "joe"
                            << "key1" << static_cast<long long int>(i * j * 37) << "key2"
                            << static_cast<long long int>(i * (645 << j))),
                Date_t()));
        }

        ASSERT_OK(mgr->writeSampleAndRotateIfNeeded(client,
                                                    BSON("name" << "joe"
                                                                << "key1" << 34 << "key2" << 45),
                                                    Date_t()));

        // Add Value
        ASSERT_OK(mgr->writeSampleAndRotateIfNeeded(client,
                                                    BSON("name" << "joe"
                                                                << "key1" << 34 << "key2" << 45),
                                                    Date_t()));
    }

    mgr->close().transitional_ignore();

    auto files = scanDirectory(dir);

    int sum = 0;
    for (auto& file : files) {
        int fs = boost::filesystem::file_size(file);
        ASSERT_TRUE(fs < c.maxFileSizeBytes * 1.10);
        LOGV2(20632, "File size", "fileName"_attr = file.generic_string(), "fileSize"_attr = fs);
        if (file.generic_string().find("interim") == std::string::npos) {
            sum += fs;
        }
    }

    ASSERT_LESS_THAN_OR_EQUALS(sum, c.maxDirectorySizeBytes * 1.10);
    ASSERT_GREATER_THAN_OR_EQUALS(sum, c.maxDirectorySizeBytes * 0.90);
}

void ValidateInterimFileHasData(const boost::filesystem::path& dir, bool hasData) {
    char buf[sizeof(std::int32_t)];

    auto interimFile = FTDCUtil::getInterimFile(dir);

    ASSERT_EQUALS(boost::filesystem::exists(interimFile), hasData);
    if (!hasData) {
        return;
    }

    std::fstream stream(interimFile.c_str());
    stream.read(&buf[0], sizeof(buf));

    ASSERT_EQUALS(4, stream.gcount());
    std::uint32_t bsonLength = ConstDataView(buf).read<LittleEndian<std::int32_t>>();

    ASSERT_EQUALS(static_cast<bool>(bsonLength), hasData);
}

// Test a normal restart
TEST_F(FTDCFileManagerTest, TestNormalRestart) {
    Client* client = &cc();
    FTDCConfig c;
    c.maxFileSizeBytes = 1000;
    c.maxDirectorySizeBytes = 3000;

    unittest::TempDir tempdir("metrics_testpath");
    boost::filesystem::path dir(tempdir.path());

    createDirectoryClean(dir);

    for (int i = 0; i < 3; i++) {
        // Do a few cases of stop and start to ensure it works as expected
        SyncFTDCCollectorCollection rotate;
        auto swMgr = FTDCFileManager::create(&c, dir, &rotate, client);
        ASSERT_OK(swMgr.getStatus());
        auto mgr = std::move(swMgr.getValue());

        // Test a large numbers of zeros, and incremental numbers in a full buffer
        for (int j = 0; j < 4; j++) {
            ASSERT_OK(mgr->writeSampleAndRotateIfNeeded(client,
                                                        BSON("name" << "joe"
                                                                    << "key1" << 3230792343LL
                                                                    << "key2" << 235135),
                                                        Date_t()));

            for (size_t i = 0; i <= FTDCConfig::kMaxSamplesPerArchiveMetricChunkDefault - 2; i++) {
                ASSERT_OK(mgr->writeSampleAndRotateIfNeeded(
                    client,
                    BSON("name" << "joe"
                                << "key1" << static_cast<long long int>(i * j * 37) << "key2"
                                << static_cast<long long int>(i * (645 << j))),
                    Date_t()));
            }

            ASSERT_OK(
                mgr->writeSampleAndRotateIfNeeded(client,
                                                  BSON("name" << "joe"
                                                              << "key1" << 34 << "key2" << 45),
                                                  Date_t()));

            // Add Value
            ASSERT_OK(
                mgr->writeSampleAndRotateIfNeeded(client,
                                                  BSON("name" << "joe"
                                                              << "key1" << 34 << "key2" << 45),
                                                  Date_t()));
        }

        mgr->close().transitional_ignore();

        // Validate the interim file does not have data
        ValidateInterimFileHasData(dir, false);
    }
}

// Test a restart after a crash with a corrupt archive file
TEST_F(FTDCFileManagerTest, TestCorruptCrashRestart) {
    Client* client = &cc();
    FTDCConfig c;
    c.maxFileSizeBytes = 1000;
    c.maxDirectorySizeBytes = 3000;

    unittest::TempDir tempdir("metrics_testpath");
    boost::filesystem::path dir(tempdir.path());

    createDirectoryClean(dir);

    for (int i = 0; i < 2; i++) {
        // Do a few cases of stop and start to ensure it works as expected
        SyncFTDCCollectorCollection rotate;
        auto swMgr = FTDCFileManager::create(&c, dir, &rotate, client);
        ASSERT_OK(swMgr.getStatus());
        auto mgr = std::move(swMgr.getValue());

        // Test a large numbers of zeros, and incremental numbers in a full buffer
        for (int j = 0; j < 4; j++) {
            ASSERT_OK(mgr->writeSampleAndRotateIfNeeded(client,
                                                        BSON("name" << "joe"
                                                                    << "key1" << 3230792343LL
                                                                    << "key2" << 235135),
                                                        Date_t()));

            for (size_t i = 0; i <= FTDCConfig::kMaxSamplesPerArchiveMetricChunkDefault - 2; i++) {
                ASSERT_OK(mgr->writeSampleAndRotateIfNeeded(
                    client,
                    BSON("name" << "joe"
                                << "key1" << static_cast<long long int>(i * j * 37) << "key2"
                                << static_cast<long long int>(i * (645 << j))),
                    Date_t()));
            }

            ASSERT_OK(
                mgr->writeSampleAndRotateIfNeeded(client,
                                                  BSON("name" << "joe"
                                                              << "key1" << 34 << "key2" << 45),
                                                  Date_t()));

            // Add Value
            ASSERT_OK(
                mgr->writeSampleAndRotateIfNeeded(client,
                                                  BSON("name" << "joe"
                                                              << "key1" << 34 << "key2" << 45),
                                                  Date_t()));
        }

        mgr->close().transitional_ignore();

        auto swFile = mgr->generateArchiveFileName(dir, "0test-crash");
        ASSERT_OK(swFile);

        std::ofstream stream(swFile.getValue().c_str());
        // This test case caused us to allocate more memory then the size of the file the first time
        // I tried it
        stream << "Hello World";

        stream.close();
    }
}

// Test a restart with a good interim file, and validate we have all the data
TEST_F(FTDCFileManagerTest, TestNormalCrashInterim) {
    Client* client = &cc();
    FTDCConfig c;
    c.maxSamplesPerInterimMetricChunk = 3;
    c.maxFileSizeBytes = 10 * 1024 * 1024;
    c.maxDirectorySizeBytes = 10 * 1024 * 1024;

    unittest::TempDir tempdir("metrics_testpath");
    boost::filesystem::path dir(tempdir.path());

    BSONObj mdoc1 = BSON("name" << "some_metadata"
                                << "key1" << 34 << "something" << 98);

    BSONObj sdoc1 = BSON("name" << "joe"
                                << "key1" << 34 << "key2" << 45);
    BSONObj sdoc2 = BSON("name" << "joe"
                                << "key3" << 34 << "key5" << 45);

    boost::filesystem::path fileOut;

    {
        SyncFTDCCollectorCollection rotate;
        auto swMgr = FTDCFileManager::create(&c, dir, &rotate, client);
        ASSERT_OK(swMgr.getStatus());
        auto swFile = swMgr.getValue()->generateArchiveFileName(dir, "0test-crash");
        ASSERT_OK(swFile);
        fileOut = swFile.getValue();
        ASSERT_OK(swMgr.getValue()->close());
    }

    createDirectoryClean(dir);

    {
        FTDCFileWriter writer(&c);

        ASSERT_OK(writer.open(fileOut));

        ASSERT_OK(writer.writeMetadata(mdoc1, Date_t()));

        ASSERT_OK(writer.writeSample(sdoc1, Date_t()));
        ASSERT_OK(writer.writeSample(sdoc1, Date_t()));
        ASSERT_OK(writer.writeSample(sdoc2, Date_t()));
        ASSERT_OK(writer.writeSample(sdoc2, Date_t()));
        ASSERT_OK(writer.writeSample(sdoc2, Date_t()));
        ASSERT_OK(writer.writeSample(sdoc2, Date_t()));

        // leave some data in the interim file
        writer.closeWithoutFlushForTest();
    }

    // Validate the interim file has data
    ValidateInterimFileHasData(dir, true);

    // Let the manager run the recovery over the interim file
    {
        SyncFTDCCollectorCollection rotate;
        auto swMgr = FTDCFileManager::create(&c, dir, &rotate, client);
        ASSERT_OK(swMgr.getStatus());
        auto mgr = std::move(swMgr.getValue());
        ASSERT_OK(mgr->close());
    }

    // Validate the file manager rolled over the changes to the current archive file
    // and did not start a new archive file.
    auto files = scanDirectory(dir);

    std::sort(files.begin(), files.end());

    // Validate old file
    std::vector<BSONObj> docs1 = {mdoc1, sdoc1, sdoc1};
    ValidateDocumentList(files[0], docs1, FTDCValidationMode::kStrict);

    // Validate new file
    std::vector<BSONObj> docs2 = {sdoc2, sdoc2, sdoc2, sdoc2};
    ValidateDocumentList(files[1], docs2, FTDCValidationMode::kStrict);
}

TEST_F(FTDCFileManagerTest, TestPeriodicMetadataCollection) {
    Client* client = &cc();
    FTDCConfig c;
    c.maxFileSizeBytes = 1024;
    c.maxSamplesPerArchiveMetricChunk = 2;    // flush every 2 samples
    c.maxSamplesPerInterimMetricChunk = 100;  // must be > maxSamplesPerArchiveMetricChunk

    unittest::TempDir tempdir("metrics_testpath");
    boost::filesystem::path dir(tempdir.path());
    createDirectoryClean(dir);

    SyncFTDCCollectorCollection rotate;
    auto mgr = uassertStatusOK(FTDCFileManager::create(&c, dir, &rotate, client));

    BSONObj subObj1 = BSON("f1_1" << 101 << "f1_2" << 102);
    BSONObj subObj2 = BSON("f2_1" << 201 << "f2_2" << 202);
    BSONObj altSubObj1 = BSON("f1_1" << 1001 << "f1_2" << 1002);
    BSONObj doc1 = BSON("field1" << subObj1 << "field2" << subObj2);
    BSONObj doc2 = BSON("field1" << altSubObj1 << "field2" << subObj2);
    BSONObj deltaDoc1 = BSON("field1" << altSubObj1);
    BSONObj deltaDoc2 = BSON("field1" << subObj1);

    int pmSamplesBeforeRotate = 0;
    auto currentFiles = scanDirectory(dir);
    size_t prevFileCount = currentFiles.size();

    const auto verifyDeltaDocuments = [&](size_t expectedSampleCount) {
        FTDCFileReader reader;
        ASSERT_EQ(currentFiles.size(), prevFileCount + 1);
        ASSERT_OK(reader.open(*(currentFiles.rbegin() + 1)));
        size_t pmSamplesRead = 0;

        while (uassertStatusOK(reader.hasNext())) {
            auto next = reader.next();
            if (std::get<0>(next) != FTDCBSONUtil::FTDCType::kPeriodicMetadata) {
                continue;
            }
            auto deltaDoc = std::get<1>(next);
            auto expectedDoc =
                (pmSamplesRead ? ((pmSamplesRead % 2) ? deltaDoc1 : deltaDoc2) : doc1);
            ASSERT_BSONOBJ_EQ(deltaDoc, expectedDoc);
            pmSamplesRead++;
        }
        ASSERT_EQ(pmSamplesRead, expectedSampleCount);
    };

    // Test writing periodic metadata samples with alternating changes until file rotates.
    while (prevFileCount == currentFiles.size()) {
        auto pmDoc = (pmSamplesBeforeRotate % 2) ? doc2 : doc1;

        ASSERT_OK(mgr->writePeriodicMetadataSampleAndRotateIfNeeded(client, pmDoc, Date_t()));
        pmSamplesBeforeRotate++;

        prevFileCount = currentFiles.size();
        currentFiles = scanDirectory(dir);
    }

    // File rotated; Read previous file, verify delta documents
    verifyDeltaDocuments(pmSamplesBeforeRotate);

    // Test writing periodic metadata samples interleaved with metric samples, until file rotates.
    prevFileCount = currentFiles.size();

    for (int iteration = 0; prevFileCount == currentFiles.size(); iteration++) {

        for (int i = 0; i < static_cast<int>(c.maxSamplesPerArchiveMetricChunk); i++) {
            ASSERT_OK(mgr->writeSampleAndRotateIfNeeded(
                client, BSON("key1" << (iteration * 37) << "key2" << (iteration * 91)), Date_t()));
        }

        // Write only 1/2 of the previous periodic samples so that rotation occurs on a metric chunk
        if (iteration < (pmSamplesBeforeRotate / 2)) {
            auto pmDoc = (iteration % 2) ? doc2 : doc1;
            ASSERT_OK(mgr->writePeriodicMetadataSampleAndRotateIfNeeded(client, pmDoc, Date_t()));
        }

        prevFileCount = currentFiles.size();
        currentFiles = scanDirectory(dir);
    }

    // File rotated; Read previous file, verify delta documents
    verifyDeltaDocuments(pmSamplesBeforeRotate / 2);

    mgr->close().transitional_ignore();
}

}  // namespace mongo
