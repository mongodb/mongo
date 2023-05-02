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

#include "mongo/db/auth/authorization_manager.h"

#include "mongo/base/init.h"
#include "mongo/base/shim.h"

namespace mongo {

SystemAuthInfo internalSecurity;

constexpr StringData AuthorizationManager::USERID_FIELD_NAME;
constexpr StringData AuthorizationManager::USER_NAME_FIELD_NAME;
constexpr StringData AuthorizationManager::USER_DB_FIELD_NAME;
constexpr StringData AuthorizationManager::ROLE_NAME_FIELD_NAME;
constexpr StringData AuthorizationManager::ROLE_DB_FIELD_NAME;
constexpr StringData AuthorizationManager::PASSWORD_FIELD_NAME;
constexpr StringData AuthorizationManager::V1_USER_NAME_FIELD_NAME;
constexpr StringData AuthorizationManager::V1_USER_SOURCE_FIELD_NAME;

const Status AuthorizationManager::authenticationFailedStatus(ErrorCodes::AuthenticationFailed,
                                                              "Authentication failed.");

const BSONObj AuthorizationManager::versionDocumentQuery = BSON("_id"
                                                                << "authSchema");

constexpr StringData AuthorizationManager::schemaVersionFieldName;

const int AuthorizationManager::schemaVersion24;
const int AuthorizationManager::schemaVersion26Upgrade;
const int AuthorizationManager::schemaVersion26Final;
const int AuthorizationManager::schemaVersion28SCRAM;

std::unique_ptr<AuthorizationManager> AuthorizationManager::create(ServiceContext* serviceContext) {
    static auto w = MONGO_WEAK_FUNCTION_DEFINITION(AuthorizationManager::create);
    return w(serviceContext);
}

}  // namespace mongo
