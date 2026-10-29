// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/auth/authorization_manager.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/shim.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"

#include <string>
#include <string_view>

namespace mongo {

SystemAuthInfo internalSecurity;

constexpr std::string_view AuthorizationManager::USERID_FIELD_NAME;
constexpr std::string_view AuthorizationManager::USER_NAME_FIELD_NAME;
constexpr std::string_view AuthorizationManager::USER_DB_FIELD_NAME;
constexpr std::string_view AuthorizationManager::ROLE_NAME_FIELD_NAME;
constexpr std::string_view AuthorizationManager::ROLE_DB_FIELD_NAME;
constexpr std::string_view AuthorizationManager::PASSWORD_FIELD_NAME;
constexpr std::string_view AuthorizationManager::V1_USER_NAME_FIELD_NAME;
constexpr std::string_view AuthorizationManager::V1_USER_SOURCE_FIELD_NAME;

const Status AuthorizationManager::authenticationFailedStatus(ErrorCodes::AuthenticationFailed,
                                                              "Authentication failed.");

const BSONObj AuthorizationManager::versionDocumentQuery = BSON("_id" << "authSchema");

}  // namespace mongo
