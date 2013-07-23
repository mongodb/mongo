/**
*    Copyright (C) 2012 10gen Inc.
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
*/

#include "mongo/db/auth/authorization_session.h"

#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authz_session_external_state.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/principal.h"
#include "mongo/db/auth/principal_set.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/privilege_set.h"
#include "mongo/db/auth/security_key.h"
#include "mongo/db/client.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

namespace {
    const std::string ADMIN_DBNAME = "admin";
}  // namespace

    AuthorizationSession::AuthorizationSession(AuthzSessionExternalState* externalState) {
        _externalState.reset(externalState);
    }

    AuthorizationSession::~AuthorizationSession(){}

    const AuthorizationManager& AuthorizationSession::getAuthorizationManager() const {
        return _externalState->getAuthorizationManager();
    }

    void AuthorizationSession::startRequest() {
        _externalState->startRequest();
    }

    void AuthorizationSession::addAuthorizedPrincipal(Principal* principal) {

        // Log out any already-logged-in user on the same database as "principal".
        logoutDatabase(principal->getName().getDB().toString());  // See SERVER-8144.

        _authenticatedPrincipals.add(principal);

        if (principal->getName() == internalSecurity.user) {

            // Grant full access to internal user
            ActionSet allActions;
            allActions.addAllActions();
            acquirePrivilege(Privilege(PrivilegeSet::WILDCARD_RESOURCE, allActions),
                             principal->getName());
            return;
        }

        const std::string dbname = principal->getName().getDB().toString();
        _acquirePrivilegesForPrincipalFromDatabase(ADMIN_DBNAME, principal->getName());
        principal->markDatabaseAsProbed(ADMIN_DBNAME);
        _acquirePrivilegesForPrincipalFromDatabase(dbname, principal->getName());
        principal->markDatabaseAsProbed(dbname);
        _externalState->onAddAuthorizedPrincipal(principal);
    }

    void AuthorizationSession::_acquirePrivilegesForPrincipalFromDatabase(
            const std::string& dbname, const UserName& user) {

        BSONObj privilegeDocument;
        Status status = _externalState->getAuthorizationManager().getPrivilegeDocument(
                dbname, user, &privilegeDocument);
        if (status.isOK()) {
            status = acquirePrivilegesFromPrivilegeDocument(dbname, user, privilegeDocument);
        }
        if (!status.isOK() && status != ErrorCodes::UserNotFound) {
            log() << "Privilege acquisition failed for " << user << " in database " <<
                dbname << ": " << status.reason() << " (" << status.codeString() << ")" << endl;
        }
    }

    Principal* AuthorizationSession::lookupPrincipal(const UserName& name) {
        return _authenticatedPrincipals.lookup(name);
    }

    void AuthorizationSession::logoutDatabase(const std::string& dbname) {
        Principal* principal = _authenticatedPrincipals.lookupByDBName(dbname);
        if (!principal)
            return;
        _acquiredPrivileges.revokePrivilegesFromUser(principal->getName());
        _authenticatedPrincipals.removeByDBName(dbname);
        _externalState->onLogoutDatabase(dbname);
    }

    PrincipalSet::NameIterator AuthorizationSession::getAuthenticatedPrincipalNames() {
        return _authenticatedPrincipals.getNames();
    }

    Status AuthorizationSession::acquirePrivilege(const Privilege& privilege,
                                                  const UserName& authorizingUser) {
        if (!_authenticatedPrincipals.lookup(authorizingUser)) {
            return Status(ErrorCodes::UserNotFound,
                          mongoutils::str::stream()
                                  << "No authenticated user found with name: "
                                  << authorizingUser.getUser()
                                  << " from database "
                                  << authorizingUser.getDB(),
                          0);
        }
        _acquiredPrivileges.grantPrivilege(privilege, authorizingUser);
        return Status::OK();
    }

    void AuthorizationSession::grantInternalAuthorization(const UserName& userName) {
        Principal* principal = new Principal(userName);
        ActionSet actions;
        actions.addAllActions();

        addAuthorizedPrincipal(principal);
        fassert(16581, acquirePrivilege(Privilege(PrivilegeSet::WILDCARD_RESOURCE, actions),
                                    principal->getName()).isOK());
    }

    bool AuthorizationSession::hasInternalAuthorization() {
        ActionSet allActions;
        allActions.addAllActions();
        return _acquiredPrivileges.hasPrivilege(Privilege(PrivilegeSet::WILDCARD_RESOURCE,
                                                          allActions));
    }

    Status AuthorizationSession::acquirePrivilegesFromPrivilegeDocument(
            const std::string& dbname, const UserName& user, const BSONObj& privilegeDocument) {
        if (!_authenticatedPrincipals.lookup(user)) {
            return Status(ErrorCodes::UserNotFound,
                          mongoutils::str::stream()
                                  << "No authenticated principle found with name: "
                                  << user.getUser()
                                  << " from database "
                                  << user.getDB(),
                          0);
        }
        return _externalState->getAuthorizationManager().buildPrivilegeSet(dbname,
                                                                           user,
                                                                           privilegeDocument,
                                                                           &_acquiredPrivileges);
    }

    bool AuthorizationSession::checkAuthorization(const std::string& resource,
                                                  ActionType action) {
        return checkAuthForPrivilege(Privilege(resource, action)).isOK();
    }

    bool AuthorizationSession::checkAuthorization(const std::string& resource,
                                                  ActionSet actions) {
        return checkAuthForPrivilege(Privilege(resource, actions)).isOK();
    }

    Status AuthorizationSession::checkAuthForQuery(const std::string& ns, const BSONObj& query) {
        NamespaceString namespaceString(ns);
        verify(!namespaceString.isCommand());
        if (!checkAuthorization(ns, ActionType::find)) {
            return Status(ErrorCodes::Unauthorized,
                          mongoutils::str::stream() << "not authorized for query on " << ns,
                          0);
        }
        return Status::OK();
    }

    Status AuthorizationSession::checkAuthForGetMore(const std::string& ns, long long cursorID) {
        if (!checkAuthorization(ns, ActionType::find)) {
            return Status(ErrorCodes::Unauthorized,
                          mongoutils::str::stream() << "not authorized for getmore on " << ns,
                          0);
        }
        return Status::OK();
    }

    Status AuthorizationSession::checkAuthForInsert(const std::string& ns,
                                                    const BSONObj& document) {
        NamespaceString namespaceString(ns);
        if (namespaceString.coll() == StringData("system.indexes", StringData::LiteralTag())) {
            std::string indexNS = document["ns"].String();
            if (!checkAuthorization(indexNS, ActionType::ensureIndex)) {
                return Status(ErrorCodes::Unauthorized,
                              mongoutils::str::stream() << "not authorized to create index on " <<
                                      indexNS,
                              0);
            }
        } else {
            if (!checkAuthorization(ns, ActionType::insert)) {
                return Status(ErrorCodes::Unauthorized,
                              mongoutils::str::stream() << "not authorized for insert on " << ns,
                              0);
            }
        }

        return Status::OK();
    }

    Status AuthorizationSession::checkAuthForUpdate(const std::string& ns,
                                                    const BSONObj& query,
                                                    const BSONObj& update,
                                                    bool upsert) {
        NamespaceString namespaceString(ns);
        if (!upsert) {
            if (!checkAuthorization(ns, ActionType::update)) {
                return Status(ErrorCodes::Unauthorized,
                              mongoutils::str::stream() << "not authorized for update on " << ns,
                              0);
            }
        }
        else {
            ActionSet required;
            required.addAction(ActionType::update);
            required.addAction(ActionType::insert);
            if (!checkAuthorization(ns, required)) {
                return Status(ErrorCodes::Unauthorized,
                              mongoutils::str::stream() << "not authorized for upsert on " << ns,
                              0);
            }
        }
        return Status::OK();
    }

    Status AuthorizationSession::checkAuthForDelete(const std::string& ns, const BSONObj& query) {
        NamespaceString namespaceString(ns);
        if (!checkAuthorization(ns, ActionType::remove)) {
            return Status(ErrorCodes::Unauthorized,
                          mongoutils::str::stream() << "not authorized to remove from " << ns,
                          0);
        }
        return Status::OK();
    }

    Privilege AuthorizationSession::_modifyPrivilegeForSpecialCases(const Privilege& privilege) {
        ActionSet newActions;
        newActions.addAllActionsFromSet(privilege.getActions());
        NamespaceString ns( privilege.getResource() );

        if (ns.coll() == "system.users") {
            if (newActions.contains(ActionType::insert) ||
                    newActions.contains(ActionType::update)) {
                // End users can't insert or update system.users directly, only the system can.
                // TODO(spencer): check for remove also once there's a command to remove users.
                newActions.addAction(ActionType::userAdminV1);
            } else {
                newActions.addAction(ActionType::userAdmin);
            }
            newActions.removeAction(ActionType::find);
            newActions.removeAction(ActionType::insert);
            newActions.removeAction(ActionType::update);
            newActions.removeAction(ActionType::remove);
        } else if (ns.coll() == "system.profile") {
            newActions.removeAction(ActionType::find);
            newActions.addAction(ActionType::profileRead);
        } else if (ns.coll() == "system.indexes" && newActions.contains(ActionType::find)) {
            newActions.removeAction(ActionType::find);
            newActions.addAction(ActionType::indexRead);
        }

        return Privilege(privilege.getResource(), newActions);
    }

    Status AuthorizationSession::checkAuthForPrivilege(const Privilege& privilege) {
        if (_externalState->shouldIgnoreAuthChecks())
            return Status::OK();

        return _probeForPrivilege(privilege);
    }

    Status AuthorizationSession::checkAuthForPrivileges(const vector<Privilege>& privileges) {
        if (_externalState->shouldIgnoreAuthChecks())
            return Status::OK();

        for (size_t i = 0; i < privileges.size(); ++i) {
            Status status = _probeForPrivilege(privileges[i]);
            if (!status.isOK())
                return status;
        }

        return Status::OK();
    }

    Status AuthorizationSession::_probeForPrivilege(const Privilege& privilege) {
        Privilege modifiedPrivilege = _modifyPrivilegeForSpecialCases(privilege);
        if (_acquiredPrivileges.hasPrivilege(modifiedPrivilege))
            return Status::OK();

        std::string dbname = nsToDatabase(modifiedPrivilege.getResource());
        for (PrincipalSet::iterator iter = _authenticatedPrincipals.begin(),
                 end = _authenticatedPrincipals.end();
             iter != end; ++iter) {

            Principal* principal = *iter;
            if (principal->isDatabaseProbed(dbname))
                continue;
            _acquirePrivilegesForPrincipalFromDatabase(dbname, principal->getName());
            principal->markDatabaseAsProbed(dbname);
            if (_acquiredPrivileges.hasPrivilege(modifiedPrivilege))
                return Status::OK();
        }
        return Status(ErrorCodes::Unauthorized, "unauthorized", 0);
    }

} // namespace mongo
