
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

#include "mongo/platform/basic.h"


#include "mongo/db/json.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/storage/storage_engine_init.h"
#include "mongo/db/storage/storage_engine_metadata.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/mongoutils/str.h"

namespace {

using namespace mongo;

class MMAPV1FactoryTest : public ServiceContextTest {
private:
    virtual void setUp() {
        ServiceContext* sc = getServiceContext();
        ASSERT_TRUE(sc);
        ASSERT_TRUE(isRegisteredStorageEngine(sc, "mmapv1"));
        factory = getFactoryForStorageEngine(sc, "mmapv1");
        ASSERT_TRUE(factory);
    }

    virtual void tearDown() {
        factory = nullptr;
    }

protected:
    const StorageEngine::Factory* factory;
};

void _testValidateMetadata(const StorageEngine::Factory* factory,
                           const BSONObj& metadataOptions,
                           bool directoryPerDB,
                           ErrorCodes::Error expectedCode) {
    // It is fine to specify an invalid data directory for the metadata
    // as long as we do not invoke read() or write().
    StorageEngineMetadata metadata("no_such_directory");
    metadata.setStorageEngineOptions(metadataOptions);

    StorageGlobalParams storageOptions;
    storageOptions.directoryperdb = directoryPerDB;

    Status status = factory->validateMetadata(metadata, storageOptions);
    if (expectedCode != status.code()) {
        FAIL(str::stream()
             << "Unexpected StorageEngine::Factory::validateMetadata result. Expected: "
             << ErrorCodes::errorString(expectedCode)
             << " but got "
             << status.toString()
             << " instead. metadataOptions: "
             << metadataOptions
             << "; directoryPerDB: "
             << directoryPerDB);
    }
}

// Do not validate fields that are not present in metadata.
TEST_F(MMAPV1FactoryTest, ValidateMetadataEmptyOptions) {
    _testValidateMetadata(factory, BSONObj(), false, ErrorCodes::OK);
    _testValidateMetadata(factory, BSONObj(), true, ErrorCodes::OK);
}

TEST_F(MMAPV1FactoryTest, ValidateMetadataDirectoryPerDB) {
    _testValidateMetadata(
        factory, fromjson("{directoryPerDB: 123}"), false, ErrorCodes::FailedToParse);
    _testValidateMetadata(factory, fromjson("{directoryPerDB: false}"), false, ErrorCodes::OK);
    _testValidateMetadata(
        factory, fromjson("{directoryPerDB: false}"), true, ErrorCodes::InvalidOptions);
    _testValidateMetadata(
        factory, fromjson("{directoryPerDB: true}"), false, ErrorCodes::InvalidOptions);
    _testValidateMetadata(factory, fromjson("{directoryPerDB: true}"), true, ErrorCodes::OK);
}

void _testCreateMetadataOptions(const StorageEngine::Factory* factory, bool directoryPerDB) {
    StorageGlobalParams storageOptions;
    storageOptions.directoryperdb = directoryPerDB;

    BSONObj metadataOptions = factory->createMetadataOptions(storageOptions);
    BSONElement directoryPerDBElement = metadataOptions.getField("directoryPerDB");
    ASSERT_TRUE(directoryPerDBElement.isBoolean());
    ASSERT_EQUALS(directoryPerDB, directoryPerDBElement.boolean());
}

TEST_F(MMAPV1FactoryTest, CreateMetadataOptions) {
    _testCreateMetadataOptions(factory, false);
    _testCreateMetadataOptions(factory, true);
}

}  // namespace
