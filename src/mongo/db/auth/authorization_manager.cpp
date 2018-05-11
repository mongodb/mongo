/**
 *    Copyright (C) 2018 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kAccessControl

#include "mongo/platform/basic.h"

#include "mongo/db/auth/authorization_manager.h"

#include <memory>
#include <string>
#include <vector>

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/bson/mutable/element.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/crypto/mechanism_scram.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/address_restriction.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/authz_manager_external_state.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/privilege_parser.h"
#include "mongo/db/auth/role_graph.h"
#include "mongo/db/auth/sasl_options.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/auth/user_document_parser.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/auth/user_name_hash.h"
#include "mongo/db/global_settings.h"
#include "mongo/db/jsobj.h"
#include "mongo/platform/compiler.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

mongo::AuthInfo mongo::internalSecurity;

namespace mongo {

const std::string AuthorizationManager::USER_NAME_FIELD_NAME = "user";
const std::string AuthorizationManager::USER_DB_FIELD_NAME = "db";
const std::string AuthorizationManager::ROLE_NAME_FIELD_NAME = "role";
const std::string AuthorizationManager::ROLE_DB_FIELD_NAME = "db";
const std::string AuthorizationManager::PASSWORD_FIELD_NAME = "pwd";
const std::string AuthorizationManager::V1_USER_NAME_FIELD_NAME = "user";
const std::string AuthorizationManager::V1_USER_SOURCE_FIELD_NAME = "userSource";

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

const std::string AuthorizationManager::schemaVersionFieldName = "currentVersion";

const int AuthorizationManager::schemaVersion24;
const int AuthorizationManager::schemaVersion26Upgrade;
const int AuthorizationManager::schemaVersion26Final;
const int AuthorizationManager::schemaVersion28SCRAM;

MONGO_DEFINE_SHIM(AuthorizationManager::create);

}  // namespace mongo
