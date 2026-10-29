// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/global_catalog/ddl/create_database_util.h"

#include "mongo/base/error_codes.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <string_view>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

TEST(CreateDatabaseUtilTest, CreateDatabaseAdminFails) {
    ASSERT_THROWS_CODE(create_database_util::checkDbNameConstraints(
                           DatabaseName::createDatabaseName_forTest(boost::none, "admin"sv)),
                       DBException,
                       ErrorCodes::InvalidOptions);

    // Alternative capitalizations are also invalid
    ASSERT_THROWS_CODE(create_database_util::checkDbNameConstraints(
                           DatabaseName::createDatabaseName_forTest(boost::none, "Admin"sv)),
                       DBException,
                       ErrorCodes::InvalidOptions);

    ASSERT_THROWS_CODE(create_database_util::checkDbNameConstraints(
                           DatabaseName::createDatabaseName_forTest(boost::none, "aDmIn"sv)),
                       DBException,
                       ErrorCodes::InvalidOptions);
}

TEST(CreateDatabaseUtilTest, CreateDatabaseLocalFails) {
    ASSERT_THROWS_CODE(create_database_util::checkDbNameConstraints(
                           DatabaseName::createDatabaseName_forTest(boost::none, "local"sv)),
                       DBException,
                       ErrorCodes::InvalidOptions);

    // Alternative capitalizations are also invalid
    ASSERT_THROWS_CODE(create_database_util::checkDbNameConstraints(
                           DatabaseName::createDatabaseName_forTest(boost::none, "Local"sv)),
                       DBException,
                       ErrorCodes::InvalidOptions);

    ASSERT_THROWS_CODE(create_database_util::checkDbNameConstraints(
                           DatabaseName::createDatabaseName_forTest(boost::none, "lOcAl"sv)),
                       DBException,
                       ErrorCodes::InvalidOptions);
}

TEST(CreateDatabaseUtilTest, CreateDatabaseConfig) {
    // It is allowed to create the "config" database.
    ASSERT_DOES_NOT_THROW(create_database_util::checkDbNameConstraints(
        DatabaseName::createDatabaseName_forTest(boost::none, "config"sv)));

    // But alternative capitalizations are invalid.
    ASSERT_THROWS_CODE(create_database_util::checkDbNameConstraints(
                           DatabaseName::createDatabaseName_forTest(boost::none, "Config"sv)),
                       DBException,
                       ErrorCodes::InvalidOptions);

    ASSERT_THROWS_CODE(create_database_util::checkDbNameConstraints(
                           DatabaseName::createDatabaseName_forTest(boost::none, "cOnFiG"sv)),
                       DBException,
                       ErrorCodes::InvalidOptions);
}

}  // namespace
}  // namespace mongo
