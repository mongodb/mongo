/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/global_catalog/ddl/create_database_util.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

namespace mongo {
namespace {

TEST(CreateDatabaseUtilTest, CreateDatabaseAdminFails) {
    ASSERT_THROWS_CODE(create_database_util::checkDbNameConstraints(
                           DatabaseName::createDatabaseName_forTest(boost::none, "admin"_sd)),
                       DBException,
                       ErrorCodes::InvalidOptions);

    // Alternative capitalizations are also invalid
    ASSERT_THROWS_CODE(create_database_util::checkDbNameConstraints(
                           DatabaseName::createDatabaseName_forTest(boost::none, "Admin"_sd)),
                       DBException,
                       ErrorCodes::InvalidOptions);

    ASSERT_THROWS_CODE(create_database_util::checkDbNameConstraints(
                           DatabaseName::createDatabaseName_forTest(boost::none, "aDmIn"_sd)),
                       DBException,
                       ErrorCodes::InvalidOptions);
}

TEST(CreateDatabaseUtilTest, CreateDatabaseLocalFails) {
    ASSERT_THROWS_CODE(create_database_util::checkDbNameConstraints(
                           DatabaseName::createDatabaseName_forTest(boost::none, "local"_sd)),
                       DBException,
                       ErrorCodes::InvalidOptions);

    // Alternative capitalizations are also invalid
    ASSERT_THROWS_CODE(create_database_util::checkDbNameConstraints(
                           DatabaseName::createDatabaseName_forTest(boost::none, "Local"_sd)),
                       DBException,
                       ErrorCodes::InvalidOptions);

    ASSERT_THROWS_CODE(create_database_util::checkDbNameConstraints(
                           DatabaseName::createDatabaseName_forTest(boost::none, "lOcAl"_sd)),
                       DBException,
                       ErrorCodes::InvalidOptions);
}

TEST(CreateDatabaseUtilTest, CreateDatabaseConfig) {
    // It is allowed to create the "config" database.
    ASSERT_DOES_NOT_THROW(create_database_util::checkDbNameConstraints(
        DatabaseName::createDatabaseName_forTest(boost::none, "config"_sd)));

    // But alternative capitalizations are invalid.
    ASSERT_THROWS_CODE(create_database_util::checkDbNameConstraints(
                           DatabaseName::createDatabaseName_forTest(boost::none, "Config"_sd)),
                       DBException,
                       ErrorCodes::InvalidOptions);

    ASSERT_THROWS_CODE(create_database_util::checkDbNameConstraints(
                           DatabaseName::createDatabaseName_forTest(boost::none, "cOnFiG"_sd)),
                       DBException,
                       ErrorCodes::InvalidOptions);
}

}  // namespace
}  // namespace mongo
