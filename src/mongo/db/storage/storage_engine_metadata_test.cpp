// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/storage/storage_engine_metadata.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"

#include <fstream>  // IWYU pragma: keep

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace {

using mongo::unittest::TempDir;
using std::string;

using namespace mongo;

TEST(StorageEngineMetadataTest, ReadNonExistentMetadataFile) {
    StorageEngineMetadata metadata("no_such_directory");
    Status status = metadata.read();
    ASSERT_NOT_OK(status);
    EXPECT_EQ(ErrorCodes::NonExistentPath, status.code());
}

TEST(StorageEngineMetadataTest, WriteToNonexistentDirectory) {
    ASSERT_NOT_OK(StorageEngineMetadata("no_such_directory").write());
}

TEST(StorageEngineMetadataTest, InvalidMetadataFileNotBSON) {
    TempDir tempDir("StorageEngineMetadataTest_InvalidMetadataFileNotBSON");
    {
        std::string filename(tempDir.path() + "/storage.bson");
        std::ofstream ofs(filename.c_str());
        // BSON document of size -1 and EOO as first element.
        BSONObj obj = fromjson("{x: 1}");
        ofs.write("\xff\xff\xff\xff", 4);
        ofs.write(obj.objdata() + 4, obj.objsize() - 4);
        ofs.flush();
    }
    {
        StorageEngineMetadata metadata(tempDir.path());
        ASSERT_NOT_OK(metadata.read());
    }
}

TEST(StorageEngineMetadataTest, InvalidMetadataFileStorageFieldMissing) {
    TempDir tempDir("StorageEngineMetadataTest_InvalidMetadataFileStorageFieldMissing");
    {
        std::string filename(tempDir.path() + "/storage.bson");
        std::ofstream ofs(filename.c_str(), std::ios_base::out | std::ios_base::binary);
        BSONObj obj = fromjson("{missing_storage_field: 123}");
        ofs.write(obj.objdata(), obj.objsize());
        ofs.flush();
    }
    {
        StorageEngineMetadata metadata(tempDir.path());
        ASSERT_NOT_OK(metadata.read());
    }
}

TEST(StorageEngineMetadataTest, InvalidMetadataFileStorageNodeNotObject) {
    TempDir tempDir("StorageEngineMetadataTest_InvalidMetadataFileStorageNodeNotObject");
    {
        std::string filename(tempDir.path() + "/storage.bson");
        std::ofstream ofs(filename.c_str());
        BSONObj obj = fromjson("{storage: 123}");
        ofs.write(obj.objdata(), obj.objsize());
        ofs.flush();
    }
    {
        StorageEngineMetadata metadata(tempDir.path());
        ASSERT_NOT_OK(metadata.read());
    }
}

TEST(StorageEngineMetadataTest, InvalidMetadataFileStorageEngineFieldMissing) {
    TempDir tempDir("StorageEngineMetadataTest_InvalidMetadataFileStorageEngineFieldMissing");
    {
        std::string filename(tempDir.path() + "/storage.bson");
        std::ofstream ofs(filename.c_str());
        BSONObj obj = fromjson("{storage: {}}");
        ofs.write(obj.objdata(), obj.objsize());
        ofs.flush();
    }
    {
        StorageEngineMetadata metadata(tempDir.path());
        ASSERT_NOT_OK(metadata.read());
    }
}

TEST(StorageEngineMetadataTest, InvalidMetadataFileStorageEngineFieldNotString) {
    TempDir tempDir("StorageEngineMetadataTest_InvalidMetadataFileStorageEngineFieldNotString");
    {
        std::string filename(tempDir.path() + "/storage.bson");
        std::ofstream ofs(filename.c_str());
        BSONObj obj = fromjson("{storage: {engine: 123}}");
        ofs.write(obj.objdata(), obj.objsize());
        ofs.flush();
    }
    {
        StorageEngineMetadata metadata(tempDir.path());
        ASSERT_NOT_OK(metadata.read());
    }
}

TEST(StorageEngineMetadataTest, InvalidMetadataFileStorageEngineOptionsFieldNotObject) {
    TempDir tempDir("StorageEngineMetadataTest_IgnoreUnknownField");
    {
        std::string filename(tempDir.path() + "/storage.bson");
        std::ofstream ofs(filename.c_str());
        BSONObj obj = fromjson("{storage: {engine: \"storageEngine1\", options: 123}}");
        ofs.write(obj.objdata(), obj.objsize());
        ofs.flush();
    }
    {
        StorageEngineMetadata metadata(tempDir.path());
        ASSERT_NOT_OK(metadata.read());
    }
}

// Metadata parser should ignore unknown metadata fields.
TEST(StorageEngineMetadataTest, IgnoreUnknownField) {
    TempDir tempDir("StorageEngineMetadataTest_IgnoreUnknownField");
    {
        std::string filename(tempDir.path() + "/storage.bson");
        std::ofstream ofs(filename.c_str());
        BSONObj obj = fromjson("{storage: {engine: \"storageEngine1\", unknown_field: 123}}");
        ofs.write(obj.objdata(), obj.objsize());
        ofs.flush();
    }
    {
        StorageEngineMetadata metadata(tempDir.path());
        ASSERT_OK(metadata.read());
        EXPECT_EQ("storageEngine1", metadata.getStorageEngine());
        EXPECT_TRUE(metadata.getStorageEngineOptions().isEmpty());
    }
}

TEST(StorageEngineMetadataTest, WriteEmptyStorageEngineName) {
    TempDir tempDir("StorageEngineMetadataTest_WriteEmptyStorageEngineName");
    StorageEngineMetadata metadata(tempDir.path());
    EXPECT_EQ("", metadata.getStorageEngine());
    // Write empty storage engine name to metadata file.
    ASSERT_NOT_OK(metadata.write());
}

TEST(StorageEngineMetadataTest, Roundtrip) {
    TempDir tempDir("StorageEngineMetadataTest_Roundtrip");
    BSONObj options = fromjson("{x: 1}");
    {
        StorageEngineMetadata metadata(tempDir.path());
        metadata.setStorageEngine("storageEngine1");
        metadata.setStorageEngineOptions(options);
        ASSERT_OK(metadata.write());
    }
    // Read back storage engine name.
    {
        StorageEngineMetadata metadata(tempDir.path());
        ASSERT_OK(metadata.read());
        EXPECT_EQ("storageEngine1", metadata.getStorageEngine());
        ASSERT_BSONOBJ_EQ(options, metadata.getStorageEngineOptions());

        metadata.reset();
        EXPECT_TRUE(metadata.getStorageEngine().empty());
        EXPECT_TRUE(metadata.getStorageEngineOptions().isEmpty());
    }
}

TEST(StorageEngineMetadataTest, ValidateStorageEngineOption) {
    // It is fine to provide an invalid data directory as long as we do not
    // call read() or write().
    StorageEngineMetadata metadata("no_such_directory");
    BSONObj options = fromjson("{x: true, y: false, z: 123}");
    metadata.setStorageEngineOptions(options);

    // Non-existent field.
    EXPECT_EQ(ErrorCodes::InvalidOptions,
              metadata.validateStorageEngineOption("w", true, boost::optional<bool>(false)).code());
    ASSERT_OK(metadata.validateStorageEngineOption("w", false, boost::optional<bool>(false)));
    ASSERT_OK(metadata.validateStorageEngineOption("w", true));
    ASSERT_OK(metadata.validateStorageEngineOption("w", false));

    // Non-boolean field.
    Status status = metadata.validateStorageEngineOption("z", true);
    ASSERT_NOT_OK(status);
    EXPECT_EQ(ErrorCodes::FailedToParse, status.code());
    status = metadata.validateStorageEngineOption("z", false);
    ASSERT_NOT_OK(status);
    EXPECT_EQ(ErrorCodes::FailedToParse, status.code());

    // Boolean fields.
    ASSERT_OK(metadata.validateStorageEngineOption("x", true));
    status = metadata.validateStorageEngineOption("x", false);
    ASSERT_NOT_OK(status);
    EXPECT_EQ(ErrorCodes::InvalidOptions, status.code());

    ASSERT_OK(metadata.validateStorageEngineOption("y", false));
    status = metadata.validateStorageEngineOption("y", true);
    ASSERT_NOT_OK(status);
    EXPECT_EQ(ErrorCodes::InvalidOptions, status.code());
}

// Do not override the active storage engine when the data directory is empty.
TEST(StorageEngineMetadataTest, StorageEngineForPath_EmptyDirectory) {
    TempDir tempDir("StorageEngineMetadataTest_StorageEngineForPath_EmptyDirectory");
    auto storageEngine = StorageEngineMetadata::getStorageEngineForPath(tempDir.path());
    EXPECT_FALSE(storageEngine);
}

// Do not override the active storage engine when the data directory is nonempty, but does not
// contain either local.ns or local/local.ns.
TEST(StorageEngineMetadataTest, StorageEngineForPath_NoDataFilesExist) {
    TempDir tempDir("StorageEngineMetadataTest_StorageEngineForPath_NoDataFilesExist");
    {
        std::string filename(tempDir.path() + "/user_data.txt");
        std::ofstream ofs(filename.c_str());
        ofs << "unused data" << std::endl;
    }
    auto storageEngine = StorageEngineMetadata::getStorageEngineForPath(tempDir.path());
    EXPECT_FALSE(storageEngine);
}

// Override the active storage engine whatever the metadata file specifies.
TEST(StorageEngineMetadataTest, StorageEngineForPath_MetadataFile_someEngine) {
    TempDir tempDir("StorageEngineMetadataTest_StorageEngineForPath_MetadataFile_someEngine");
    {
        StorageEngineMetadata metadata(tempDir.path());
        metadata.setStorageEngine("someEngine");
        ASSERT_OK(metadata.write());
    }
    EXPECT_EQ(std::string("someEngine"),
              StorageEngineMetadata::getStorageEngineForPath(tempDir.path()));
}

}  // namespace
