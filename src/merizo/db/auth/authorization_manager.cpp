/**
 *    Copyright (C) 2018-present MerizoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MerizoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.merizodb.com/licensing/server-side-public-license>.
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

#define MERIZO_LOG_DEFAULT_COMPONENT ::merizo::logger::LogComponent::kAccessControl

#include "merizo/platform/basic.h"

#include "merizo/db/auth/authorization_manager.h"

#include <memory>
#include <string>
#include <vector>

#include "merizo/base/init.h"
#include "merizo/base/status.h"
#include "merizo/bson/mutable/document.h"
#include "merizo/bson/mutable/element.h"
#include "merizo/bson/util/bson_extract.h"
#include "merizo/crypto/mechanism_scram.h"
#include "merizo/db/auth/action_set.h"
#include "merizo/db/auth/address_restriction.h"
#include "merizo/db/auth/authorization_session.h"
#include "merizo/db/auth/authz_manager_external_state.h"
#include "merizo/db/auth/privilege.h"
#include "merizo/db/auth/privilege_parser.h"
#include "merizo/db/auth/role_graph.h"
#include "merizo/db/auth/sasl_options.h"
#include "merizo/db/auth/user.h"
#include "merizo/db/auth/user_document_parser.h"
#include "merizo/db/auth/user_name.h"
#include "merizo/db/global_settings.h"
#include "merizo/db/jsobj.h"
#include "merizo/platform/compiler.h"
#include "merizo/stdx/memory.h"
#include "merizo/stdx/mutex.h"
#include "merizo/stdx/unordered_map.h"
#include "merizo/util/assert_util.h"
#include "merizo/util/log.h"
#include "merizo/util/merizoutils/str.h"

merizo::AuthInfo merizo::internalSecurity;

namespace merizo {

constexpr StringData AuthorizationManager::USERID_FIELD_NAME;
constexpr StringData AuthorizationManager::USER_NAME_FIELD_NAME;
constexpr StringData AuthorizationManager::USER_DB_FIELD_NAME;
constexpr StringData AuthorizationManager::ROLE_NAME_FIELD_NAME;
constexpr StringData AuthorizationManager::ROLE_DB_FIELD_NAME;
constexpr StringData AuthorizationManager::PASSWORD_FIELD_NAME;
constexpr StringData AuthorizationManager::V1_USER_NAME_FIELD_NAME;
constexpr StringData AuthorizationManager::V1_USER_SOURCE_FIELD_NAME;


const NamespaceString AuthorizationManager::adminCommandNamespace("admin.$cmd");
const NamespaceString AuthorizationManager::rolesCollectionNamespace("admin.system.roles");
const NamespaceString AuthorizationManager::usersAltCollectionNamespace("admin.system.new_users");
const NamespaceString AuthorizationManager::usersBackupCollectionNamespace(
    "admin.system.backup_users");
const NamespaceString AuthorizationManager::usersCollectionNamespace("admin.system.users");
const NamespaceString AuthorizationManager::versionCollectionNamespace("admin.system.version");
const NamespaceString AuthorizationManager::defaultTempUsersCollectionNamespace("admin.tempusers");
const NamespaceString AuthorizationManager::defaultTempRolesCollectionNamespace("admin.temproles");

const Status AuthorizationManager::authenticationFailedStatus(ErrorCodes::AuthenticationFailed,
                                                              "Authentication failed.");

const BSONObj AuthorizationManager::versionDocumentQuery = BSON("_id"
                                                                << "authSchema");

constexpr StringData AuthorizationManager::schemaVersionFieldName;

const int AuthorizationManager::schemaVersion24;
const int AuthorizationManager::schemaVersion26Upgrade;
const int AuthorizationManager::schemaVersion26Final;
const int AuthorizationManager::schemaVersion28SCRAM;

MERIZO_DEFINE_SHIM(AuthorizationManager::create);

}  // namespace merizo
