/**
*    Copyright (C) 2013 10gen Inc.
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

#include "mongo/base/disallow_copying.h"
#include "mongo/base/init.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/service_context.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {
class AuthzVersionParameter : public ServerParameter {
    MONGO_DISALLOW_COPYING(AuthzVersionParameter);

public:
    AuthzVersionParameter(ServerParameterSet* sps, const std::string& name);
    virtual void append(OperationContext* txn, BSONObjBuilder& b, const std::string& name);
    virtual Status set(const BSONElement& newValueElement);
    virtual Status setFromString(const std::string& str);
};

MONGO_INITIALIZER_GENERAL(AuthzSchemaParameter,
                          MONGO_NO_PREREQUISITES,
                          ("BeginStartupOptionParsing"))
(InitializerContext*) {
    new AuthzVersionParameter(ServerParameterSet::getGlobal(), authSchemaVersionServerParameter);
    return Status::OK();
}

AuthzVersionParameter::AuthzVersionParameter(ServerParameterSet* sps, const std::string& name)
    : ServerParameter(sps, name, false, false) {}

void AuthzVersionParameter::append(OperationContext* txn,
                                   BSONObjBuilder& b,
                                   const std::string& name) {
    int authzVersion;
    uassertStatusOK(getGlobalAuthorizationManager()->getAuthorizationVersion(txn, &authzVersion));
    b.append(name, authzVersion);
}

Status AuthzVersionParameter::set(const BSONElement& newValueElement) {
    return Status(ErrorCodes::InternalError, "set called on unsettable server parameter");
}

Status AuthzVersionParameter::setFromString(const std::string& newValueString) {
    return Status(ErrorCodes::InternalError, "set called on unsettable server parameter");
}

}  // namespace

const std::string authSchemaVersionServerParameter = "authSchemaVersion";

AuthorizationManager* getGlobalAuthorizationManager() {
    AuthorizationManager* globalAuthManager = AuthorizationManager::get(getGlobalServiceContext());
    fassert(16842, globalAuthManager != nullptr);
    return globalAuthManager;
}

}  // namespace mongo
