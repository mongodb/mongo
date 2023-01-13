/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/s/config/config_server_test_fixture.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"

namespace mongo {
namespace {

class ShardingCatalogManagerDatabaseOperationsTest : public ConfigServerTestFixture {
public:
    void setUp() override {
        ConfigServerTestFixture::setUp();
        _opCtx = operationContext();
    }

protected:
    OperationContext* _opCtx;
};

TEST_F(ShardingCatalogManagerDatabaseOperationsTest, CreateDatabaseAdminFails) {
    ASSERT_THROWS_CODE(
        ShardingCatalogManager::get(_opCtx)->createDatabase(_opCtx, "admin"_sd, boost::none),
        DBException,
        ErrorCodes::InvalidOptions);

    // Alternative capitalizations are also invalid
    ASSERT_THROWS_CODE(
        ShardingCatalogManager::get(_opCtx)->createDatabase(_opCtx, "Admin"_sd, boost::none),
        DBException,
        ErrorCodes::InvalidOptions);

    ASSERT_THROWS_CODE(
        ShardingCatalogManager::get(_opCtx)->createDatabase(_opCtx, "aDmIn"_sd, boost::none),
        DBException,
        ErrorCodes::InvalidOptions);
}

TEST_F(ShardingCatalogManagerDatabaseOperationsTest, CreateDatabaseLocalFails) {
    ASSERT_THROWS_CODE(
        ShardingCatalogManager::get(_opCtx)->createDatabase(_opCtx, "local"_sd, boost::none),
        DBException,
        ErrorCodes::InvalidOptions);

    // Alternative capitalizations are also invalid
    ASSERT_THROWS_CODE(
        ShardingCatalogManager::get(_opCtx)->createDatabase(_opCtx, "Local"_sd, boost::none),
        DBException,
        ErrorCodes::InvalidOptions);

    ASSERT_THROWS_CODE(
        ShardingCatalogManager::get(_opCtx)->createDatabase(_opCtx, "lOcAl"_sd, boost::none),
        DBException,
        ErrorCodes::InvalidOptions);
}

TEST_F(ShardingCatalogManagerDatabaseOperationsTest, CreateDatabaseConfig) {
    // It is allowed to create the "config" database.
    ASSERT_DOES_NOT_THROW(
        ShardingCatalogManager::get(_opCtx)->createDatabase(_opCtx, "config"_sd, boost::none));

    // But alternative capitalizations are invalid.
    ASSERT_THROWS_CODE(
        ShardingCatalogManager::get(_opCtx)->createDatabase(_opCtx, "Config"_sd, boost::none),
        DBException,
        ErrorCodes::InvalidOptions);

    ASSERT_THROWS_CODE(
        ShardingCatalogManager::get(_opCtx)->createDatabase(_opCtx, "cOnFiG"_sd, boost::none),
        DBException,
        ErrorCodes::InvalidOptions);
}

}  // namespace
}  // namespace mongo
