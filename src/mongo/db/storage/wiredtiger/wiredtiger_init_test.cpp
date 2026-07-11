// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_engine_init.h"
#include "mongo/db/storage/storage_engine_metadata.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_global_options.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/str.h"

namespace {

using namespace mongo;

class WiredTigerFactoryTest : public ServiceContextTest {
private:
    void setUp() override {
        ServiceContext* globalEnv = getGlobalServiceContext();
        ASSERT_TRUE(globalEnv);
        ASSERT_TRUE(isRegisteredStorageEngine(globalEnv, kWiredTigerEngineName));
        factory = getFactoryForStorageEngine(globalEnv, kWiredTigerEngineName);
        ASSERT_TRUE(factory);
        _oldOptions = wiredTigerGlobalOptions;
    }

    void tearDown() override {
        wiredTigerGlobalOptions = _oldOptions;
        factory = nullptr;
    }

    WiredTigerGlobalOptions _oldOptions;

protected:
    const StorageEngine::Factory* factory;
};

void _testValidateMetadata(const StorageEngine::Factory* factory,
                           const BSONObj& metadataOptions,
                           bool directoryPerDB,
                           bool directoryForIndexes,
                           ErrorCodes::Error expectedCode) {
    // It is fine to specify an invalid data directory for the metadata
    // as long as we do not invoke read() or write().
    StorageEngineMetadata metadata("no_such_directory");
    metadata.setStorageEngineOptions(metadataOptions);

    StorageGlobalParams storageOptions;
    storageOptions.directoryperdb = directoryPerDB;
    wiredTigerGlobalOptions.directoryForIndexes = directoryForIndexes;

    Status status = factory->validateMetadata(metadata, storageOptions);
    if (expectedCode != status.code()) {
        FAIL(std::string(str::stream()
                         << "Unexpected StorageEngine::Factory::validateMetadata result. Expected: "
                         << ErrorCodes::errorString(expectedCode) << " but got "
                         << status.toString() << " instead. metadataOptions: " << metadataOptions
                         << "; directoryPerDB: " << directoryPerDB
                         << "; directoryForIndexes: " << directoryForIndexes));
    }
}

// Do not validate fields that are not present in metadata.
TEST_F(WiredTigerFactoryTest, ValidateMetadataEmptyOptions) {
    _testValidateMetadata(factory, BSONObj(), false, false, ErrorCodes::OK);
    _testValidateMetadata(factory, BSONObj(), false, true, ErrorCodes::OK);
    _testValidateMetadata(factory, BSONObj(), true, false, ErrorCodes::OK);
    _testValidateMetadata(factory, BSONObj(), false, false, ErrorCodes::OK);
}

TEST_F(WiredTigerFactoryTest, ValidateMetadataDirectoryPerDB) {
    _testValidateMetadata(
        factory, fromjson("{directoryPerDB: 123}"), false, false, ErrorCodes::FailedToParse);
    _testValidateMetadata(
        factory, fromjson("{directoryPerDB: false}"), false, false, ErrorCodes::OK);
    _testValidateMetadata(
        factory, fromjson("{directoryPerDB: false}"), true, false, ErrorCodes::InvalidOptions);
    _testValidateMetadata(
        factory, fromjson("{directoryPerDB: true}"), false, false, ErrorCodes::InvalidOptions);
    _testValidateMetadata(factory, fromjson("{directoryPerDB: true}"), true, false, ErrorCodes::OK);
}

TEST_F(WiredTigerFactoryTest, ValidateMetadataDirectoryForIndexes) {
    _testValidateMetadata(
        factory, fromjson("{directoryForIndexes: 123}"), false, false, ErrorCodes::FailedToParse);
    _testValidateMetadata(
        factory, fromjson("{directoryForIndexes: false}"), false, false, ErrorCodes::OK);
    _testValidateMetadata(
        factory, fromjson("{directoryForIndexes: false}"), false, true, ErrorCodes::InvalidOptions);
    _testValidateMetadata(
        factory, fromjson("{directoryForIndexes: true}"), false, false, ErrorCodes::InvalidOptions);
    _testValidateMetadata(
        factory, fromjson("{directoryForIndexes: true}"), true, true, ErrorCodes::OK);
}

void _testCreateMetadataOptions(const StorageEngine::Factory* factory,
                                bool directoryPerDB,
                                bool directoryForIndexes) {
    StorageGlobalParams storageOptions;
    storageOptions.directoryperdb = directoryPerDB;
    wiredTigerGlobalOptions.directoryForIndexes = directoryForIndexes;

    BSONObj metadataOptions = factory->createMetadataOptions(storageOptions);

    BSONElement directoryPerDBElement = metadataOptions.getField("directoryPerDB");
    ASSERT_TRUE(directoryPerDBElement.isBoolean());
    ASSERT_EQUALS(directoryPerDB, directoryPerDBElement.boolean());

    BSONElement directoryForIndexesElement = metadataOptions.getField("directoryForIndexes");
    ASSERT_TRUE(directoryForIndexesElement.isBoolean());
    ASSERT_EQUALS(directoryForIndexes, directoryForIndexesElement.boolean());
}

TEST_F(WiredTigerFactoryTest, CreateMetadataOptions) {
    _testCreateMetadataOptions(factory, false, false);
    _testCreateMetadataOptions(factory, false, true);
    _testCreateMetadataOptions(factory, true, false);
    _testCreateMetadataOptions(factory, true, true);
}

}  // namespace
