/**
 *    Copyright 2014 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include <fstream>
#include <ios>
#include <ostream>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/storage/storage_engine_metadata.h"
#include "mongo/db/json.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"

namespace {

    using std::string;
    using mongo::unittest::TempDir;

    using namespace mongo;

    TEST(StorageEngineMetadataTest, ReadNonExistentMetadataFile) {
        StorageEngineMetadata metadata("no_such_directory");
        Status status = metadata.read();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::NonExistentPath, status.code());
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
            ofs.write(obj.objdata()+4, obj.objsize()-4);
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
            ASSERT_EQUALS("storageEngine1", metadata.getStorageEngine());
        }
    }

    TEST(StorageEngineMetadataTest, WriteEmptyStorageEngineName) {
        TempDir tempDir("StorageEngineMetadataTest_WriteEmptyStorageEngineName");
        StorageEngineMetadata metadata(tempDir.path());
        ASSERT_EQUALS("", metadata.getStorageEngine());
        // Write empty storage engine name to metadata file.
        ASSERT_NOT_OK(metadata.write());
    }

    TEST(StorageEngineMetadataTest, Roundtrip) {
        TempDir tempDir("StorageEngineMetadataTest_Roundtrip");
        {
            StorageEngineMetadata metadata(tempDir.path());
            metadata.setStorageEngine("storageEngine1");
            ASSERT_OK(metadata.write());
        }
        // Read back storage engine name.
        {
            StorageEngineMetadata metadata(tempDir.path());
            ASSERT_OK(metadata.read());
            ASSERT_EQUALS("storageEngine1", metadata.getStorageEngine());
        }
    }

    TEST(StorageEngineMetadataTest, ValidateEmptyDirectory) {
        TempDir tempDir("StorageEngineMetadataTest_ValidateEmptyDirectory");
        StorageEngineMetadata::validate(tempDir.path(), "storageEngine1");
    }

    // Data directory is not empty but metadata is missing.
    // Current storage engine is 'mmapv1'.
    TEST(StorageEngineMetadataTest, ValidateMissingMetadataStorageEngineMMapV1) {
        TempDir tempDir("StorageEngineMetadataTest_ValidateMissingMetadataStorageEngineMMapV1");
        {
            std::string filename(tempDir.path() + "/local.ns");
            std::ofstream ofs(filename.c_str());
            ofs << "unused data" << std::endl;
        }
        StorageEngineMetadata::validate(tempDir.path(), "mmapv1");
    }

    // Data directory is not empty but metadata is missing.
    // Current storage engine is not 'mmapv1'.
    TEST(StorageEngineMetadataTest, ValidateMissingMetadataStorageEngineNotMMapV1) {
        TempDir tempDir("StorageEngineMetadataTest_ValidateMissingMetadataStorageEngineNotMMapV1");
        {
            std::string filename(tempDir.path() + "/local.ns");
            std::ofstream ofs(filename.c_str());
            ofs << "unused data" << std::endl;
        }
        ASSERT_THROWS(StorageEngineMetadata::validate(tempDir.path(), "engine1"), UserException);
    }

    TEST(StorageEngineMetadataTest, ValidateMetadataMatchesStorageEngine) {
        TempDir tempDir("StorageEngineMetadataTest_ValidateMetadataMatchesStorageEngine");
        {
            StorageEngineMetadata metadata(tempDir.path());
            metadata.setStorageEngine("storageEngine1");
            ASSERT_OK(metadata.write());
        }
        StorageEngineMetadata::validate(tempDir.path(), "storageEngine1");
    }

    TEST(StorageEngineMetadataTest, ValidateMetadataDifferentFromStorageEngine) {
        TempDir tempDir("StorageEngineMetadataTest_ValidateMetadataDifferentFromStorageEngine");
        {
            StorageEngineMetadata metadata(tempDir.path());
            metadata.setStorageEngine("storageEngine1");
            ASSERT_OK(metadata.write());
        }
        ASSERT_THROWS(StorageEngineMetadata::validate(tempDir.path(), "engine2"), UserException);
    }

    TEST(StorageEngineMetadataTest, ValidateMetadataReadFailed) {
        TempDir tempDir("StorageEngineMetadataTest_ValidateMetadataReadFailed");
        {
            std::string filename(tempDir.path() + "/storage.bson");
            std::ofstream ofs(filename.c_str());
            // BSON document of size -1 and EOO as first element.
            BSONObj obj = fromjson("{x: 1}");
            ofs.write("\xff\xff\xff\xff", 4);
            ofs.write(obj.objdata()+4, obj.objsize()-4);
            ofs.flush();
        }
        ASSERT_THROWS(StorageEngineMetadata::validate(tempDir.path(), "engine2"), UserException);
    }

    TEST(StorageEngineMetadataTest, UpdateIfMissingNoExistingMetadata) {
        TempDir tempDir("StorageEngineMetadataTest_UpdateIfMissingNoExistingMetadata");
        ASSERT_OK(StorageEngineMetadata::updateIfMissing(tempDir.path(), "storageEngine1"));
        // Read back storage engine name.
        {
            StorageEngineMetadata metadata(tempDir.path());
            ASSERT_OK(metadata.read());
            ASSERT_EQUALS("storageEngine1", metadata.getStorageEngine());
        }
    }

    // Do not overwrite existing metadata file.
    TEST(StorageEngineMetadataTest, UpdateIfMissingMetadataExists) {
        TempDir tempDir("StorageEngineMetadataTest_UpdateIfMissingMetadataExists");
        {
            StorageEngineMetadata metadata(tempDir.path());
            metadata.setStorageEngine("storageEngine1");
            ASSERT_OK(metadata.write());
        }
        // Invoke update() with a different storage engine name.
        // This should not modify the existings file.
        ASSERT_OK(StorageEngineMetadata::updateIfMissing(tempDir.path(), "storageEngine2"));
        // Read back storage engine name.
        {
            StorageEngineMetadata metadata(tempDir.path());
            ASSERT_OK(metadata.read());
            ASSERT_EQUALS("storageEngine1", metadata.getStorageEngine());
        }
    }

    TEST(StorageEngineMetadataTest, UpdateIfMissingInvalidDirectory) {
        // Should warn and return write() status if we fail to update the metadata file.
        ASSERT_NOT_OK(StorageEngineMetadata::updateIfMissing("no_such_directory", "engine1"));
    }

}  // namespace
