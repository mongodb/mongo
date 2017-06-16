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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kAccessControl

#include "mongo/platform/basic.h"

#include "mongo/db/auth/authorization_session.h"

#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authz_session_external_state.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/security_key.h"
#include "mongo/db/auth/user_management_commands_parser.h"
#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/client.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/server_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

namespace dps = ::mongo::dotted_path_support;
using std::vector;

namespace {
const std::string ADMIN_DBNAME = "admin";

// Checks if this connection has the privileges necessary to create or modify the view 'viewNs'
// to be a view on 'viewOnNs' with pipeline 'viewPipeline'. Call this function after verifying
// that the user has the 'createCollection' or 'collMod' action, respectively.
Status checkAuthForCreateOrModifyView(AuthorizationSession* authzSession,
                                      const NamespaceString& viewNs,
                                      const NamespaceString& viewOnNs,
                                      const BSONArray& viewPipeline,
                                      bool isMongos) {
    // It's safe to allow a user to create or modify a view if they can't read it anyway.
    if (!authzSession->isAuthorizedForActionsOnNamespace(viewNs, ActionType::find)) {
        return Status::OK();
    }

    // This check ignores some invalid pipeline specifications. For example, if a user specifies a
    // view definition with an invalid specification like {$lookup: "blah"}, the authorization check
    // will succeed but the pipeline will fail to parse later in Command::run().
    return authzSession->checkAuthForAggregate(
        viewOnNs, BSON("aggregate" << viewOnNs.coll() << "pipeline" << viewPipeline), isMongos);
}
}  // namespace

AuthorizationSession::AuthorizationSession(std::unique_ptr<AuthzSessionExternalState> externalState)
    : _externalState(std::move(externalState)), _impersonationFlag(false) {}

AuthorizationSession::~AuthorizationSession() {
    for (UserSet::iterator it = _authenticatedUsers.begin(); it != _authenticatedUsers.end();
         ++it) {
        getAuthorizationManager().releaseUser(*it);
    }
}

AuthorizationManager& AuthorizationSession::getAuthorizationManager() {
    return _externalState->getAuthorizationManager();
}

void AuthorizationSession::startRequest(OperationContext* opCtx) {
    _externalState->startRequest(opCtx);
    _refreshUserInfoAsNeeded(opCtx);
}

Status AuthorizationSession::addAndAuthorizeUser(OperationContext* opCtx,
                                                 const UserName& userName) {
    User* user;
    Status status = getAuthorizationManager().acquireUserForInitialAuth(opCtx, userName, &user);
    if (!status.isOK()) {
        return status;
    }

    // Calling add() on the UserSet may return a user that was replaced because it was from the
    // same database.
    User* replacedUser = _authenticatedUsers.add(user);
    if (replacedUser) {
        getAuthorizationManager().releaseUser(replacedUser);
    }

    // If there are any users and roles in the impersonation data, clear it out.
    clearImpersonatedUserData();

    _buildAuthenticatedRolesVector();
    return Status::OK();
}

User* AuthorizationSession::lookupUser(const UserName& name) {
    return _authenticatedUsers.lookup(name);
}

void AuthorizationSession::logoutDatabase(const std::string& dbname) {
    User* removedUser = _authenticatedUsers.removeByDBName(dbname);
    if (removedUser) {
        getAuthorizationManager().releaseUser(removedUser);
    }
    clearImpersonatedUserData();
    _buildAuthenticatedRolesVector();
}

UserNameIterator AuthorizationSession::getAuthenticatedUserNames() {
    return _authenticatedUsers.getNames();
}

RoleNameIterator AuthorizationSession::getAuthenticatedRoleNames() {
    return makeRoleNameIterator(_authenticatedRoleNames.begin(), _authenticatedRoleNames.end());
}

std::string AuthorizationSession::getAuthenticatedUserNamesToken() {
    std::string ret;
    for (UserNameIterator nameIter = getAuthenticatedUserNames(); nameIter.more();
         nameIter.next()) {
        ret += '\0';  // Using a NUL byte which isn't valid in usernames to separate them.
        ret += nameIter->getFullName();
    }

    return ret;
}

void AuthorizationSession::grantInternalAuthorization() {
    _authenticatedUsers.add(internalSecurity.user);
    _buildAuthenticatedRolesVector();
}

PrivilegeVector AuthorizationSession::getDefaultPrivileges() {
    PrivilegeVector defaultPrivileges;

    // If localhost exception is active (and no users exist),
    // return a vector of the minimum privileges required to bootstrap
    // a system and add the first user.
    if (_externalState->shouldAllowLocalhost()) {
        ResourcePattern adminDBResource = ResourcePattern::forDatabaseName(ADMIN_DBNAME);
        ActionSet setupAdminUserActionSet;
        setupAdminUserActionSet.addAction(ActionType::createUser);
        setupAdminUserActionSet.addAction(ActionType::grantRole);
        Privilege setupAdminUserPrivilege = Privilege(adminDBResource, setupAdminUserActionSet);

        ResourcePattern externalDBResource = ResourcePattern::forDatabaseName("$external");
        Privilege setupExternalUserPrivilege =
            Privilege(externalDBResource, ActionType::createUser);

        ActionSet setupServerConfigActionSet;

        // If this server is an arbiter, add specific privileges meant to circumvent
        // the behavior of an arbiter in an authenticated replset. See SERVER-5479.
        if (_externalState->serverIsArbiter()) {
            setupServerConfigActionSet.addAction(ActionType::getCmdLineOpts);
            setupServerConfigActionSet.addAction(ActionType::getParameter);
            setupServerConfigActionSet.addAction(ActionType::serverStatus);
            setupServerConfigActionSet.addAction(ActionType::shutdown);
        }

        setupServerConfigActionSet.addAction(ActionType::addShard);
        setupServerConfigActionSet.addAction(ActionType::replSetConfigure);
        setupServerConfigActionSet.addAction(ActionType::replSetGetStatus);
        Privilege setupServerConfigPrivilege =
            Privilege(ResourcePattern::forClusterResource(), setupServerConfigActionSet);

        Privilege::addPrivilegeToPrivilegeVector(&defaultPrivileges, setupAdminUserPrivilege);
        Privilege::addPrivilegeToPrivilegeVector(&defaultPrivileges, setupExternalUserPrivilege);
        Privilege::addPrivilegeToPrivilegeVector(&defaultPrivileges, setupServerConfigPrivilege);
        return defaultPrivileges;
    }

    return defaultPrivileges;
}

void AuthorizationSession::_addPrivilegesForStage(const std::string& db,
                                                  const BSONObj& cmdObj,
                                                  PrivilegeVector* requiredPrivileges,
                                                  BSONObj stageSpec,
                                                  bool haveRecursed) {
    StringData stageName = stageSpec.firstElementFieldName();
    if (stageName == "$out" && stageSpec.firstElementType() == BSONType::String) {
        NamespaceString outputNs(db, stageSpec.firstElement().str());
        uassert(17139,
                mongoutils::str::stream() << "Invalid $out target namespace, " << outputNs.ns(),
                outputNs.isValid());

        ActionSet actions;
        actions.addAction(ActionType::remove);
        actions.addAction(ActionType::insert);
        if (shouldBypassDocumentValidationForCommand(cmdObj)) {
            actions.addAction(ActionType::bypassDocumentValidation);
        }
        Privilege::addPrivilegeToPrivilegeVector(
            requiredPrivileges, Privilege(ResourcePattern::forExactNamespace(outputNs), actions));
    } else if (stageName == "$lookup" && stageSpec.firstElementType() == BSONType::Object) {
        NamespaceString fromNs(db, stageSpec.firstElement()["from"].str());
        Privilege::addPrivilegeToPrivilegeVector(
            requiredPrivileges,
            Privilege(ResourcePattern::forExactNamespace(fromNs), ActionType::find));
    } else if (stageName == "$graphLookup" && stageSpec.firstElementType() == BSONType::Object) {
        NamespaceString fromNs(db, stageSpec.firstElement()["from"].str());
        Privilege::addPrivilegeToPrivilegeVector(
            requiredPrivileges,
            Privilege(ResourcePattern::forExactNamespace(fromNs), ActionType::find));
    } else if (stageName == "$facet" && stageSpec.firstElementType() == BSONType::Object &&
               !haveRecursed) {
        // Add privileges of sub-stages, but only if we haven't recursed already. We don't want to
        // get a stack overflow while checking privileges. If we ever allow a $facet stage inside of
        // a $facet stage, this code will have to be modified to avoid causing a stack overflow, but
        // still check all required privileges of nested stages.
        for (auto&& subPipeline : stageSpec.firstElement().embeddedObject()) {
            if (subPipeline.type() == BSONType::Array) {
                for (auto&& subPipeStageSpec : subPipeline.embeddedObject()) {
                    _addPrivilegesForStage(db,
                                           cmdObj,
                                           requiredPrivileges,
                                           subPipeStageSpec.embeddedObjectUserCheck(),
                                           true);
                }
            }
        }
    }
}

Status AuthorizationSession::checkAuthForAggregate(const NamespaceString& ns,
                                                   const BSONObj& cmdObj,
                                                   bool isMongos) {
    std::string db(ns.db().toString());
    uassert(
        17138, mongoutils::str::stream() << "Invalid input namespace, " << ns.ns(), ns.isValid());

    // If this connection does not need to be authenticated (for instance, if auth is disabled),
    // return Status::OK() immediately.
    if (_externalState->shouldIgnoreAuthChecks()) {
        return Status::OK();
    }

    PrivilegeVector privileges;

    BSONElement pipelineElem = cmdObj["pipeline"];
    if (pipelineElem.type() != BSONType::Array) {
        return Status(ErrorCodes::TypeMismatch, "'pipeline' must be specified as an array");
    }

    BSONObj pipeline = pipelineElem.embeddedObject();
    if (pipeline.isEmpty()) {
        // The pipeline is empty, so we require only the find action.
        Privilege::addPrivilegeToPrivilegeVector(
            &privileges, Privilege(ResourcePattern::forExactNamespace(ns), ActionType::find));
    } else {
        if (pipeline.firstElementType() != BSONType::Object) {
            // The pipeline contains something that's not an object.
            return Status(ErrorCodes::TypeMismatch,
                          "'pipeline' cannot contain non-object elements");
        }

        // We treat the first stage in the pipeline specially, as some aggregation stages that are
        // valid initial sources have different auth requirements.
        BSONObj firstPipelineStage = pipeline.firstElement().embeddedObject();
        BSONElement firstStageSpec = firstPipelineStage.firstElement();
        if (str::equals("$indexStats", firstStageSpec.fieldName())) {
            Privilege::addPrivilegeToPrivilegeVector(
                &privileges,
                Privilege(ResourcePattern::forExactNamespace(ns), ActionType::indexStats));
        } else if (str::equals("$collStats", firstStageSpec.fieldName())) {
            Privilege::addPrivilegeToPrivilegeVector(
                &privileges,
                Privilege(ResourcePattern::forExactNamespace(ns), ActionType::collStats));
        } else if (str::equals("$currentOp", firstStageSpec.fieldName())) {
            // Need to check the value of allUsers; if true then inprog privilege is required.
            // {$currentOp: {idleConnections: <boolean|false>, allUsers: <boolean|false>}}
            if (firstStageSpec.type() != BSONType::Object) {
                return Status(
                    ErrorCodes::TypeMismatch,
                    str::stream()
                        << "$currentOp options must be specified in an object, but found: "
                        << typeName(firstStageSpec.type()));
            }

            bool allUsers = false;

            // Check the spec for all fields named 'allUsers'. If any of them are 'true', we require
            // the 'inprog' privilege. This avoids the possibility that a spec with multiple
            // allUsers fields might allow an unauthorized user to view all operations.
            for (auto&& elem : firstStageSpec.embeddedObject()) {
                if (elem.fieldNameStringData() == "allUsers"_sd) {
                    if (elem.type() != BSONType::Bool) {
                        return Status(ErrorCodes::TypeMismatch,
                                      str::stream()
                                          << "The 'allUsers' parameter of the $currentOp stage "
                                             "must be a boolean value, but found: "
                                          << typeName(elem.type()));
                    } else if (elem.Bool()) {
                        allUsers = true;
                        break;
                    }
                }
            }

            // In a sharded cluster, we always need the inprog privilege to run $currentOp.
            if (isMongos || allUsers) {
                Privilege::addPrivilegeToPrivilegeVector(
                    &privileges,
                    Privilege(ResourcePattern::forClusterResource(), ActionType::inprog));
            } else if (!getAuthenticatedUserNames().more()) {
                // This connection is not authenticated, so we should return an error even though
                // there are no privilege requirements when allUsers is false.
                return Status(ErrorCodes::Unauthorized, "unauthorized");
            }
        } else {
            // If no source requiring an alternative permission scheme is specified then default to
            // requiring find() privileges on the given namespace.
            Privilege::addPrivilegeToPrivilegeVector(
                &privileges, Privilege(ResourcePattern::forExactNamespace(ns), ActionType::find));
        }

        // Add additional required privileges for each stage in the pipeline.
        for (auto&& stageElem : pipeline) {
            _addPrivilegesForStage(db, cmdObj, &privileges, stageElem.embeddedObjectUserCheck());
        }
    }

    if (isAuthorizedForPrivileges(privileges))
        return Status::OK();
    return Status(ErrorCodes::Unauthorized, "unauthorized");
}

Status AuthorizationSession::checkAuthForFind(const NamespaceString& ns, bool hasTerm) {
    if (MONGO_unlikely(ns.isCommand())) {
        return Status(ErrorCodes::InternalError,
                      str::stream() << "Checking query auth on command namespace " << ns.ns());
    }
    if (!isAuthorizedForActionsOnNamespace(ns, ActionType::find)) {
        return Status(ErrorCodes::Unauthorized,
                      str::stream() << "not authorized for query on " << ns.ns());
    }

    // Only internal clients (such as other nodes in a replica set) are allowed to use
    // the 'term' field in a find operation. Use of this field could trigger changes
    // in the receiving server's replication state and should be protected.
    if (hasTerm &&
        !isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                          ActionType::internal)) {
        return Status(ErrorCodes::Unauthorized,
                      str::stream() << "not authorized for query with term on " << ns.ns());
    }

    return Status::OK();
}

Status AuthorizationSession::checkAuthForGetMore(const NamespaceString& ns,
                                                 long long cursorID,
                                                 bool hasTerm) {
    // "ns" can be in one of three formats: "listCollections" format, "listIndexes" format, and
    // normal format.
    if (ns.isListCollectionsCursorNS()) {
        // "ns" is of the form "<db>.$cmd.listCollections".  Check if we can perform the
        // listCollections action on the database resource for "<db>".
        if (!isAuthorizedToListCollections(ns.db())) {
            return Status(ErrorCodes::Unauthorized,
                          str::stream() << "not authorized for listCollections getMore on "
                                        << ns.ns());
        }
    } else if (ns.isListIndexesCursorNS()) {
        // "ns" is of the form "<db>.$cmd.listIndexes.<coll>".  Check if we can perform the
        // listIndexes action on the "<db>.<coll>" namespace.
        NamespaceString targetNS = ns.getTargetNSForListIndexes();
        if (!isAuthorizedForActionsOnNamespace(targetNS, ActionType::listIndexes)) {
            return Status(ErrorCodes::Unauthorized,
                          str::stream() << "not authorized for listIndexes getMore on " << ns.ns());
        }
    } else {
        // "ns" is a regular namespace string.  Check if we can perform the find action on it.
        if (!isAuthorizedForActionsOnNamespace(ns, ActionType::find)) {
            return Status(ErrorCodes::Unauthorized,
                          str::stream() << "not authorized for getMore on " << ns.ns());
        }
    }

    // Only internal clients (such as other nodes in a replica set) are allowed to use
    // the 'term' field in a getMore operation. Use of this field could trigger changes
    // in the receiving server's replication state and should be protected.
    if (hasTerm &&
        !isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                          ActionType::internal)) {
        return Status(ErrorCodes::Unauthorized,
                      str::stream() << "not authorized for getMore with term on " << ns.ns());
    }

    return Status::OK();
}

Status AuthorizationSession::checkAuthForInsert(OperationContext* opCtx,
                                                const NamespaceString& ns,
                                                const BSONObj& document) {
    if (ns.coll() == "system.indexes"_sd) {
        BSONElement nsElement = document["ns"];
        if (nsElement.type() != String) {
            return Status(nsElement.type() == BSONType::EOO ? ErrorCodes::NoSuchKey
                                                            : ErrorCodes::TypeMismatch,
                          "Cannot authorize inserting into "
                          "system.indexes documents without a string-typed \"ns\" field.");
        }
        NamespaceString indexNS(nsElement.str());
        if (!isAuthorizedForActionsOnNamespace(indexNS, ActionType::createIndex)) {
            return Status(ErrorCodes::Unauthorized,
                          str::stream() << "not authorized to create index on " << indexNS.ns());
        }
    } else {
        ActionSet required{ActionType::insert};
        if (documentValidationDisabled(opCtx)) {
            required.addAction(ActionType::bypassDocumentValidation);
        }
        if (!isAuthorizedForActionsOnNamespace(ns, required)) {
            return Status(ErrorCodes::Unauthorized,
                          str::stream() << "not authorized for insert on " << ns.ns());
        }
    }

    return Status::OK();
}

Status AuthorizationSession::checkAuthForUpdate(OperationContext* opCtx,
                                                const NamespaceString& ns,
                                                const BSONObj& query,
                                                const BSONObj& update,
                                                bool upsert) {
    ActionSet required{ActionType::update};
    StringData operationType = "update"_sd;

    if (upsert) {
        required.addAction(ActionType::insert);
        operationType = "upsert"_sd;
    }

    if (documentValidationDisabled(opCtx)) {
        required.addAction(ActionType::bypassDocumentValidation);
    }

    if (!isAuthorizedForActionsOnNamespace(ns, required)) {
        return Status(ErrorCodes::Unauthorized,
                      str::stream() << "not authorized for " << operationType << " on " << ns.ns());
    }

    return Status::OK();
}

Status AuthorizationSession::checkAuthForDelete(OperationContext* opCtx,
                                                const NamespaceString& ns,
                                                const BSONObj& query) {
    if (!isAuthorizedForActionsOnNamespace(ns, ActionType::remove)) {
        return Status(ErrorCodes::Unauthorized,
                      str::stream() << "not authorized to remove from " << ns.ns());
    }
    return Status::OK();
}

Status AuthorizationSession::checkAuthForKillCursors(const NamespaceString& ns,
                                                     long long cursorID) {
    // See implementation comments in checkAuthForGetMore().  This method looks very similar.

    // SERVER-20364 Check for find or killCursor privileges until we have a way of associating
    // a cursor with an owner.
    if (ns.isListCollectionsCursorNS()) {
        if (!(isAuthorizedForActionsOnResource(ResourcePattern::forDatabaseName(ns.db()),
                                               ActionType::killCursors) ||
              isAuthorizedToListCollections(ns.db()))) {
            return Status(ErrorCodes::Unauthorized,
                          str::stream() << "not authorized to kill listCollections cursor on "
                                        << ns.ns());
        }
    } else if (ns.isListIndexesCursorNS()) {
        NamespaceString targetNS = ns.getTargetNSForListIndexes();
        if (!(isAuthorizedForActionsOnNamespace(targetNS, ActionType::killCursors) ||
              isAuthorizedForActionsOnNamespace(targetNS, ActionType::listIndexes))) {
            return Status(ErrorCodes::Unauthorized,
                          str::stream() << "not authorized to kill listIndexes cursor on "
                                        << ns.ns());
        }
    } else {
        if (!(isAuthorizedForActionsOnNamespace(ns, ActionType::killCursors) ||
              isAuthorizedForActionsOnNamespace(ns, ActionType::find))) {
            return Status(ErrorCodes::Unauthorized,
                          str::stream() << "not authorized to kill cursor on " << ns.ns());
        }
    }
    return Status::OK();
}

Status AuthorizationSession::checkAuthForCreate(const NamespaceString& ns,
                                                const BSONObj& cmdObj,
                                                bool isMongos) {
    if (cmdObj["capped"].trueValue() &&
        !isAuthorizedForActionsOnNamespace(ns, ActionType::convertToCapped)) {
        return Status(ErrorCodes::Unauthorized, "unauthorized");
    }

    const bool hasCreateCollectionAction =
        isAuthorizedForActionsOnNamespace(ns, ActionType::createCollection);

    // If attempting to create a view, check for additional required privileges.
    if (cmdObj["viewOn"]) {
        // You need the createCollection action on this namespace; the insert action is not
        // sufficient.
        if (!hasCreateCollectionAction) {
            return Status(ErrorCodes::Unauthorized, "unauthorized");
        }

        // Parse the viewOn namespace and the pipeline. If no pipeline was specified, use the empty
        // pipeline.
        NamespaceString viewOnNs(ns.db(), cmdObj["viewOn"].checkAndGetStringData());
        auto pipeline =
            cmdObj.hasField("pipeline") ? BSONArray(cmdObj["pipeline"].Obj()) : BSONArray();
        return checkAuthForCreateOrModifyView(this, ns, viewOnNs, pipeline, isMongos);
    }

    // To create a regular collection, ActionType::createCollection or ActionType::insert are
    // both acceptable.
    if (hasCreateCollectionAction || isAuthorizedForActionsOnNamespace(ns, ActionType::insert)) {
        return Status::OK();
    }

    return Status(ErrorCodes::Unauthorized, "unauthorized");
}

Status AuthorizationSession::checkAuthForCollMod(const NamespaceString& ns,
                                                 const BSONObj& cmdObj,
                                                 bool isMongos) {
    if (!isAuthorizedForActionsOnNamespace(ns, ActionType::collMod)) {
        return Status(ErrorCodes::Unauthorized, "unauthorized");
    }

    // Check for additional required privileges if attempting to modify a view. When auth is
    // enabled, users must specify both "viewOn" and "pipeline" together. This prevents a user from
    // exposing more information in the original underlying namespace by only changing "pipeline",
    // or looking up more information via the original pipeline by only changing "viewOn".
    const bool hasViewOn = cmdObj.hasField("viewOn");
    const bool hasPipeline = cmdObj.hasField("pipeline");
    if (hasViewOn != hasPipeline) {
        return Status(
            ErrorCodes::InvalidOptions,
            "Must specify both 'viewOn' and 'pipeline' when modifying a view and auth is enabled");
    }
    if (hasViewOn) {
        NamespaceString viewOnNs(ns.db(), cmdObj["viewOn"].checkAndGetStringData());
        auto viewPipeline = BSONArray(cmdObj["pipeline"].Obj());
        return checkAuthForCreateOrModifyView(this, ns, viewOnNs, viewPipeline, isMongos);
    }

    return Status::OK();
}

Status AuthorizationSession::checkAuthorizedToGrantPrivilege(const Privilege& privilege) {
    const ResourcePattern& resource = privilege.getResourcePattern();
    if (resource.isDatabasePattern() || resource.isExactNamespacePattern()) {
        if (!isAuthorizedForActionsOnResource(
                ResourcePattern::forDatabaseName(resource.databaseToMatch()),
                ActionType::grantRole)) {
            return Status(ErrorCodes::Unauthorized,
                          str::stream() << "Not authorized to grant privileges on the "
                                        << resource.databaseToMatch()
                                        << "database");
        }
    } else if (!isAuthorizedForActionsOnResource(ResourcePattern::forDatabaseName("admin"),
                                                 ActionType::grantRole)) {
        return Status(ErrorCodes::Unauthorized,
                      "To grant privileges affecting multiple databases or the cluster,"
                      " must be authorized to grant roles from the admin database");
    }
    return Status::OK();
}


Status AuthorizationSession::checkAuthorizedToRevokePrivilege(const Privilege& privilege) {
    const ResourcePattern& resource = privilege.getResourcePattern();
    if (resource.isDatabasePattern() || resource.isExactNamespacePattern()) {
        if (!isAuthorizedForActionsOnResource(
                ResourcePattern::forDatabaseName(resource.databaseToMatch()),
                ActionType::revokeRole)) {
            return Status(ErrorCodes::Unauthorized,
                          str::stream() << "Not authorized to revoke privileges on the "
                                        << resource.databaseToMatch()
                                        << "database");
        }
    } else if (!isAuthorizedForActionsOnResource(ResourcePattern::forDatabaseName("admin"),
                                                 ActionType::revokeRole)) {
        return Status(ErrorCodes::Unauthorized,
                      "To revoke privileges affecting multiple databases or the cluster,"
                      " must be authorized to revoke roles from the admin database");
    }
    return Status::OK();
}

bool AuthorizationSession::isAuthorizedToCreateRole(
    const struct auth::CreateOrUpdateRoleArgs& args) {
    // A user is allowed to create a role under either of two conditions.

    // The user may create a role if the authorization system says they are allowed to.
    if (isAuthorizedForActionsOnResource(ResourcePattern::forDatabaseName(args.roleName.getDB()),
                                         ActionType::createRole)) {
        return true;
    }

    // The user may create a role if the localhost exception is enabled, and they already own the
    // role. This implies they have obtained the role through an external authorization mechanism.
    if (_externalState->shouldAllowLocalhost()) {
        for (const User* const user : _authenticatedUsers) {
            if (user->hasRole(args.roleName)) {
                return true;
            }
        }
        log() << "Not authorized to create the first role in the system '" << args.roleName
              << "' using the localhost exception. The user needs to acquire the role through "
                 "external authentication first.";
    }

    return false;
}

bool AuthorizationSession::isAuthorizedToGrantRole(const RoleName& role) {
    return isAuthorizedForActionsOnResource(ResourcePattern::forDatabaseName(role.getDB()),
                                            ActionType::grantRole);
}

bool AuthorizationSession::isAuthorizedToRevokeRole(const RoleName& role) {
    return isAuthorizedForActionsOnResource(ResourcePattern::forDatabaseName(role.getDB()),
                                            ActionType::revokeRole);
}

bool AuthorizationSession::isAuthorizedForPrivilege(const Privilege& privilege) {
    if (_externalState->shouldIgnoreAuthChecks())
        return true;

    return _isAuthorizedForPrivilege(privilege);
}

bool AuthorizationSession::isAuthorizedForPrivileges(const vector<Privilege>& privileges) {
    if (_externalState->shouldIgnoreAuthChecks())
        return true;

    for (size_t i = 0; i < privileges.size(); ++i) {
        if (!_isAuthorizedForPrivilege(privileges[i]))
            return false;
    }

    return true;
}

bool AuthorizationSession::isAuthorizedForActionsOnResource(const ResourcePattern& resource,
                                                            ActionType action) {
    return isAuthorizedForPrivilege(Privilege(resource, action));
}

bool AuthorizationSession::isAuthorizedForActionsOnResource(const ResourcePattern& resource,
                                                            const ActionSet& actions) {
    return isAuthorizedForPrivilege(Privilege(resource, actions));
}

bool AuthorizationSession::isAuthorizedForActionsOnNamespace(const NamespaceString& ns,
                                                             ActionType action) {
    return isAuthorizedForPrivilege(Privilege(ResourcePattern::forExactNamespace(ns), action));
}

bool AuthorizationSession::isAuthorizedForActionsOnNamespace(const NamespaceString& ns,
                                                             const ActionSet& actions) {
    return isAuthorizedForPrivilege(Privilege(ResourcePattern::forExactNamespace(ns), actions));
}

static const int resourceSearchListCapacity = 5;
/**
 * Builds from "target" an exhaustive list of all ResourcePatterns that match "target".
 *
 * Stores the resulting list into resourceSearchList, and returns the length.
 *
 * The seach lists are as follows, depending on the type of "target":
 *
 * target is ResourcePattern::forAnyResource():
 *   searchList = { ResourcePattern::forAnyResource(), ResourcePattern::forAnyResource() }
 * target is the ResourcePattern::forClusterResource():
 *   searchList = { ResourcePattern::forAnyResource(), ResourcePattern::forClusterResource() }
 * target is a database, db:
 *   searchList = { ResourcePattern::forAnyResource(),
 *                  ResourcePattern::forAnyNormalResource(),
 *                  db }
 * target is a non-system collection, db.coll:
 *   searchList = { ResourcePattern::forAnyResource(),
 *                  ResourcePattern::forAnyNormalResource(),
 *                  db,
 *                  coll,
 *                  db.coll }
 * target is a system collection, db.system.coll:
 *   searchList = { ResourcePattern::forAnyResource(),
 *                  system.coll,
 *                  db.system.coll }
 */
static int buildResourceSearchList(const ResourcePattern& target,
                                   ResourcePattern resourceSearchList[resourceSearchListCapacity]) {
    int size = 0;
    resourceSearchList[size++] = ResourcePattern::forAnyResource();
    if (target.isExactNamespacePattern()) {
        if (!target.ns().isSystem()) {
            // Some databases should not be matchable with ResourcePattern::forAnyNormalResource.
            // 'local' and 'config' are used to store special system collections, which user level
            // administrators should not be able to manipulate.
            if (target.ns().db() != "local" && target.ns().db() != "config") {
                resourceSearchList[size++] = ResourcePattern::forAnyNormalResource();
            }
            resourceSearchList[size++] = ResourcePattern::forDatabaseName(target.ns().db());
        }
        resourceSearchList[size++] = ResourcePattern::forCollectionName(target.ns().coll());
    } else if (target.isDatabasePattern()) {
        resourceSearchList[size++] = ResourcePattern::forAnyNormalResource();
    }
    resourceSearchList[size++] = target;
    dassert(size <= resourceSearchListCapacity);
    return size;
}

bool AuthorizationSession::isAuthorizedToChangeAsUser(const UserName& userName,
                                                      ActionType actionType) {
    User* user = lookupUser(userName);
    if (!user) {
        return false;
    }
    ResourcePattern resourceSearchList[resourceSearchListCapacity];
    const int resourceSearchListLength = buildResourceSearchList(
        ResourcePattern::forDatabaseName(userName.getDB()), resourceSearchList);

    ActionSet actions;
    for (int i = 0; i < resourceSearchListLength; ++i) {
        actions.addAllActionsFromSet(user->getActionsForResource(resourceSearchList[i]));
    }
    return actions.contains(actionType);
}

bool AuthorizationSession::isAuthorizedToChangeOwnPasswordAsUser(const UserName& userName) {
    return AuthorizationSession::isAuthorizedToChangeAsUser(userName,
                                                            ActionType::changeOwnPassword);
}

bool AuthorizationSession::isAuthorizedToChangeOwnCustomDataAsUser(const UserName& userName) {
    return AuthorizationSession::isAuthorizedToChangeAsUser(userName,
                                                            ActionType::changeOwnCustomData);
}

bool AuthorizationSession::isAuthorizedToListCollections(StringData dbname) {
    // Check for the listCollections ActionType on the database or find on system.namespaces for
    // pre 3.0 systems.

    return AuthorizationSession::isAuthorizedForActionsOnResource(
               ResourcePattern::forDatabaseName(dbname), ActionType::listCollections) ||
        AuthorizationSession::isAuthorizedForActionsOnResource(
               ResourcePattern::forExactNamespace(NamespaceString(dbname, "system.namespaces")),
               ActionType::find);
}

bool AuthorizationSession::isAuthenticatedAsUserWithRole(const RoleName& roleName) {
    for (UserSet::iterator it = _authenticatedUsers.begin(); it != _authenticatedUsers.end();
         ++it) {
        if ((*it)->hasRole(roleName)) {
            return true;
        }
    }
    return false;
}

void AuthorizationSession::_refreshUserInfoAsNeeded(OperationContext* opCtx) {
    AuthorizationManager& authMan = getAuthorizationManager();
    UserSet::iterator it = _authenticatedUsers.begin();
    while (it != _authenticatedUsers.end()) {
        User* user = *it;

        if (!user->isValid()) {
            // Make a good faith effort to acquire an up-to-date user object, since the one
            // we've cached is marked "out-of-date."
            UserName name = user->getName();
            User* updatedUser;

            Status status =
                authMan.acquireUserToRefreshSessionCache(opCtx, name, user->getID(), &updatedUser);

            switch (status.code()) {
                case ErrorCodes::OK: {
                    // Success! Replace the old User object with the updated one.
                    fassert(17067, _authenticatedUsers.replaceAt(it, updatedUser) == user);
                    authMan.releaseUser(user);
                    LOG(1) << "Updated session cache of user information for " << name;
                    break;
                }
                case ErrorCodes::UserNotFound: {
                    // User does not exist anymore; remove it from _authenticatedUsers.
                    fassert(17068, _authenticatedUsers.removeAt(it) == user);
                    authMan.releaseUser(user);
                    log() << "Removed deleted user " << name
                          << " from session cache of user information.";
                    continue;  // No need to advance "it" in this case.
                }
                default:
                    // Unrecognized error; assume that it's transient, and continue working with the
                    // out-of-date privilege data.
                    warning() << "Could not fetch updated user privilege information for " << name
                              << "; continuing to use old information.  Reason is "
                              << redact(status);
                    break;
            }
        }
        ++it;
    }
    _buildAuthenticatedRolesVector();
}

void AuthorizationSession::_buildAuthenticatedRolesVector() {
    _authenticatedRoleNames.clear();
    for (UserSet::iterator it = _authenticatedUsers.begin(); it != _authenticatedUsers.end();
         ++it) {
        RoleNameIterator roles = (*it)->getIndirectRoles();
        while (roles.more()) {
            RoleName roleName = roles.next();
            _authenticatedRoleNames.push_back(RoleName(roleName.getRole(), roleName.getDB()));
        }
    }
}

bool AuthorizationSession::_isAuthorizedForPrivilege(const Privilege& privilege) {
    const ResourcePattern& target(privilege.getResourcePattern());

    ResourcePattern resourceSearchList[resourceSearchListCapacity];
    const int resourceSearchListLength = buildResourceSearchList(target, resourceSearchList);

    ActionSet unmetRequirements = privilege.getActions();

    PrivilegeVector defaultPrivileges = getDefaultPrivileges();
    for (PrivilegeVector::iterator it = defaultPrivileges.begin(); it != defaultPrivileges.end();
         ++it) {
        for (int i = 0; i < resourceSearchListLength; ++i) {
            if (!(it->getResourcePattern() == resourceSearchList[i]))
                continue;

            ActionSet userActions = it->getActions();
            unmetRequirements.removeAllActionsFromSet(userActions);

            if (unmetRequirements.empty())
                return true;
        }
    }

    for (UserSet::iterator it = _authenticatedUsers.begin(); it != _authenticatedUsers.end();
         ++it) {
        User* user = *it;
        for (int i = 0; i < resourceSearchListLength; ++i) {
            ActionSet userActions = user->getActionsForResource(resourceSearchList[i]);
            unmetRequirements.removeAllActionsFromSet(userActions);

            if (unmetRequirements.empty())
                return true;
        }
    }

    return false;
}

void AuthorizationSession::setImpersonatedUserData(std::vector<UserName> usernames,
                                                   std::vector<RoleName> roles) {
    _impersonatedUserNames = usernames;
    _impersonatedRoleNames = roles;
    _impersonationFlag = true;
}

bool AuthorizationSession::isCoauthorizedWithClient(Client* opClient) {
    auto getUserNames = [](AuthorizationSession* authSession) {
        if (authSession->isImpersonating()) {
            return authSession->getImpersonatedUserNames();
        } else {
            return authSession->getAuthenticatedUserNames();
        }
    };

    UserNameIterator it = getUserNames(this);
    while (it.more()) {
        UserNameIterator opIt = getUserNames(AuthorizationSession::get(opClient));
        while (opIt.more()) {
            if (it.get() == opIt.get()) {
                return true;
            }
            opIt.next();
        }
        it.next();
    }

    return false;
}

bool AuthorizationSession::isCoauthorizedWith(UserNameIterator userNameIter) {
    if (!getAuthorizationManager().isAuthEnabled()) {
        return true;
    }
    if (!userNameIter.more() && !getAuthenticatedUserNames().more()) {
        return true;
    }

    for (; userNameIter.more(); userNameIter.next()) {
        for (UserNameIterator thisUserNameIter = getAuthenticatedUserNames();
             thisUserNameIter.more();
             thisUserNameIter.next()) {
            if (*userNameIter == *thisUserNameIter) {
                return true;
            }
        }
    }

    return false;
}

UserNameIterator AuthorizationSession::getImpersonatedUserNames() {
    return makeUserNameIterator(_impersonatedUserNames.begin(), _impersonatedUserNames.end());
}

RoleNameIterator AuthorizationSession::getImpersonatedRoleNames() {
    return makeRoleNameIterator(_impersonatedRoleNames.begin(), _impersonatedRoleNames.end());
}

// Clear the vectors of impersonated usernames and roles.
void AuthorizationSession::clearImpersonatedUserData() {
    _impersonatedUserNames.clear();
    _impersonatedRoleNames.clear();
    _impersonationFlag = false;
}


bool AuthorizationSession::isImpersonating() const {
    return _impersonationFlag;
}

}  // namespace mongo
