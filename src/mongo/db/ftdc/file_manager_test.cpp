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

#include <algorithm>
#include <boost/filesystem.hpp>
#include <iostream>
#include <string>

#include "mongo/base/init.h"
#include "mongo/bson/bson_validate.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/ftdc/collector.h"
#include "mongo/db/ftdc/config.h"
#include "mongo/db/ftdc/file_manager.h"
#include "mongo/db/ftdc/file_writer.h"
#include "mongo/db/ftdc/ftdc_test.h"
#include "mongo/db/jsobj.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

// Test a full buffer
TEST(FTDCFileManagerTest, TestFull) {
    Client* client = &cc();
    FTDCConfig c;
    c.maxFileSizeBytes = 300;
    c.maxDirectorySizeBytes = 1000;
    c.maxSamplesPerInterimMetricChunk = 1;

    unittest::TempDir tempdir("metrics_testpath");
    boost::filesystem::path dir(tempdir.path());
    createDirectoryClean(dir);

    FTDCCollectorCollection rotate;
    auto swMgr = FTDCFileManager::create(&c, dir, &rotate, client);
    ASSERT_OK(swMgr.getStatus());
    auto mgr = std::move(swMgr.getValue());

    // Test a large numbers of zeros, and incremental numbers in a full buffer
    for (int j = 0; j < 10; j++) {
        ASSERT_OK(mgr->writeSampleAndRotateIfNeeded(client,
                                                    BSON("name"
                                                         << "joe"
                                                         << "key1"
                                                         << 3230792343LL
                                                         << "key2"
                                                         << 235135),
                                                    Date_t()));

        for (size_t i = 0; i <= FTDCConfig::kMaxSamplesPerArchiveMetricChunkDefault - 2; i++) {
            ASSERT_OK(
                mgr->writeSampleAndRotateIfNeeded(client,
                                                  BSON("name"
                                                       << "joe"
                                                       << "key1"
                                                       << static_cast<long long int>(i * j * 37)
                                                       << "key2"
                                                       << static_cast<long long int>(i *
                                                                                     (645 << j))),
                                                  Date_t()));
        }

        ASSERT_OK(mgr->writeSampleAndRotateIfNeeded(client,
                                                    BSON("name"
                                                         << "joe"
                                                         << "key1"
                                                         << 34
                                                         << "key2"
                                                         << 45),
                                                    Date_t()));

        // Add Value
        ASSERT_OK(mgr->writeSampleAndRotateIfNeeded(client,
                                                    BSON("name"
                                                         << "joe"
                                                         << "key1"
                                                         << 34
                                                         << "key2"
                                                         << 45),
                                                    Date_t()));
    }

    mgr->close();

    auto files = scanDirectory(dir);

    int sum = 0;
    for (auto& file : files) {
        int fs = boost::filesystem::file_size(file);
        ASSERT_TRUE(fs < c.maxFileSizeBytes * 1.10);
        unittest::log() << "File " << file.generic_string() << " has size " << fs;
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
TEST(FTDCFileManagerTest, TestNormalRestart) {
    Client* client = &cc();
    FTDCConfig c;
    c.maxFileSizeBytes = 1000;
    c.maxDirectorySizeBytes = 3000;

    unittest::TempDir tempdir("metrics_testpath");
    boost::filesystem::path dir(tempdir.path());

    createDirectoryClean(dir);

    for (int i = 0; i < 3; i++) {
        // Do a few cases of stop and start to ensure it works as expected
        FTDCCollectorCollection rotate;
        auto swMgr = FTDCFileManager::create(&c, dir, &rotate, client);
        ASSERT_OK(swMgr.getStatus());
        auto mgr = std::move(swMgr.getValue());

        // Test a large numbers of zeros, and incremental numbers in a full buffer
        for (int j = 0; j < 4; j++) {
            ASSERT_OK(mgr->writeSampleAndRotateIfNeeded(client,
                                                        BSON("name"
                                                             << "joe"
                                                             << "key1"
                                                             << 3230792343LL
                                                             << "key2"
                                                             << 235135),
                                                        Date_t()));

            for (size_t i = 0; i <= FTDCConfig::kMaxSamplesPerArchiveMetricChunkDefault - 2; i++) {
                ASSERT_OK(
                    mgr->writeSampleAndRotateIfNeeded(
                        client,
                        BSON("name"
                             << "joe"
                             << "key1"
                             << static_cast<long long int>(i * j * 37)
                             << "key2"
                             << static_cast<long long int>(i * (645 << j))),
                        Date_t()));
            }

            ASSERT_OK(mgr->writeSampleAndRotateIfNeeded(client,
                                                        BSON("name"
                                                             << "joe"
                                                             << "key1"
                                                             << 34
                                                             << "key2"
                                                             << 45),
                                                        Date_t()));

            // Add Value
            ASSERT_OK(mgr->writeSampleAndRotateIfNeeded(client,
                                                        BSON("name"
                                                             << "joe"
                                                             << "key1"
                                                             << 34
                                                             << "key2"
                                                             << 45),
                                                        Date_t()));
        }

        mgr->close();

        // Validate the interim file does not have data
        ValidateInterimFileHasData(dir, false);
    }
}

// Test a restart after a crash with a corrupt archive file
TEST(FTDCFileManagerTest, TestCorruptCrashRestart) {
    Client* client = &cc();
    FTDCConfig c;
    c.maxFileSizeBytes = 1000;
    c.maxDirectorySizeBytes = 3000;

    unittest::TempDir tempdir("metrics_testpath");
    boost::filesystem::path dir(tempdir.path());

    createDirectoryClean(dir);

    for (int i = 0; i < 2; i++) {
        // Do a few cases of stop and start to ensure it works as expected
        FTDCCollectorCollection rotate;
        auto swMgr = FTDCFileManager::create(&c, dir, &rotate, client);
        ASSERT_OK(swMgr.getStatus());
        auto mgr = std::move(swMgr.getValue());

        // Test a large numbers of zeros, and incremental numbers in a full buffer
        for (int j = 0; j < 4; j++) {
            ASSERT_OK(mgr->writeSampleAndRotateIfNeeded(client,
                                                        BSON("name"
                                                             << "joe"
                                                             << "key1"
                                                             << 3230792343LL
                                                             << "key2"
                                                             << 235135),
                                                        Date_t()));

            for (size_t i = 0; i <= FTDCConfig::kMaxSamplesPerArchiveMetricChunkDefault - 2; i++) {
                ASSERT_OK(
                    mgr->writeSampleAndRotateIfNeeded(
                        client,
                        BSON("name"
                             << "joe"
                             << "key1"
                             << static_cast<long long int>(i * j * 37)
                             << "key2"
                             << static_cast<long long int>(i * (645 << j))),
                        Date_t()));
            }

            ASSERT_OK(mgr->writeSampleAndRotateIfNeeded(client,
                                                        BSON("name"
                                                             << "joe"
                                                             << "key1"
                                                             << 34
                                                             << "key2"
                                                             << 45),
                                                        Date_t()));

            // Add Value
            ASSERT_OK(mgr->writeSampleAndRotateIfNeeded(client,
                                                        BSON("name"
                                                             << "joe"
                                                             << "key1"
                                                             << 34
                                                             << "key2"
                                                             << 45),
                                                        Date_t()));
        }

        mgr->close();

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
TEST(FTDCFileManagerTest, TestNormalCrashInterim) {
    Client* client = &cc();
    FTDCConfig c;
    c.maxSamplesPerInterimMetricChunk = 3;
    c.maxFileSizeBytes = 10 * 1024 * 1024;
    c.maxDirectorySizeBytes = 10 * 1024 * 1024;

    unittest::TempDir tempdir("metrics_testpath");
    boost::filesystem::path dir(tempdir.path());

    BSONObj mdoc1 = BSON("name"
                         << "some_metadata"
                         << "key1"
                         << 34
                         << "something"
                         << 98);

    BSONObj sdoc1 = BSON("name"
                         << "joe"
                         << "key1"
                         << 34
                         << "key2"
                         << 45);
    BSONObj sdoc2 = BSON("name"
                         << "joe"
                         << "key3"
                         << 34
                         << "key5"
                         << 45);

    boost::filesystem::path fileOut;

    {
        FTDCCollectorCollection rotate;
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
        FTDCCollectorCollection rotate;
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
    ValidateDocumentList(files[0], docs1);

    // Validate new file
    std::vector<BSONObj> docs2 = {sdoc2, sdoc2, sdoc2, sdoc2};
    ValidateDocumentList(files[1], docs2);
}

}  // namespace mongo
