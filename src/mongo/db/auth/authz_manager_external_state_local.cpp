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

#include "mongo/db/auth/authz_manager_external_state_local.h"

#include "mongo/base/status.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/bson/mutable/element.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/privilege_parser.h"
#include "mongo/db/auth/user_document_parser.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_options.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using std::vector;

Status AuthzManagerExternalStateLocal::initialize(OperationContext* opCtx) {
    Status status = _initializeRoleGraph(opCtx);
    if (!status.isOK()) {
        if (status == ErrorCodes::GraphContainsCycle) {
            error() << "Cycle detected in admin.system.roles; role inheritance disabled. "
                       "Remove the listed cycle and any others to re-enable role inheritance. "
                    << redact(status);
        } else {
            error() << "Could not generate role graph from admin.system.roles; "
                       "only system roles available: "
                    << redact(status);
        }
    }

    return Status::OK();
}

Status AuthzManagerExternalStateLocal::getStoredAuthorizationVersion(OperationContext* opCtx,
                                                                     int* outVersion) {
    BSONObj versionDoc;
    Status status = findOne(opCtx,
                            AuthorizationManager::versionCollectionNamespace,
                            AuthorizationManager::versionDocumentQuery,
                            &versionDoc);
    if (status.isOK()) {
        BSONElement versionElement = versionDoc[AuthorizationManager::schemaVersionFieldName];
        if (versionElement.isNumber()) {
            *outVersion = versionElement.numberInt();
            return Status::OK();
        } else if (versionElement.eoo()) {
            return Status(ErrorCodes::NoSuchKey,
                          mongoutils::str::stream() << "No "
                                                    << AuthorizationManager::schemaVersionFieldName
                                                    << " field in version document.");
        } else {
            return Status(ErrorCodes::TypeMismatch,
                          mongoutils::str::stream()
                              << "Could not determine schema version of authorization data.  "
                                 "Bad (non-numeric) type "
                              << typeName(versionElement.type())
                              << " ("
                              << versionElement.type()
                              << ") for "
                              << AuthorizationManager::schemaVersionFieldName
                              << " field in version document");
        }
    } else if (status == ErrorCodes::NoMatchingDocument) {
        *outVersion = AuthorizationManager::schemaVersion28SCRAM;
        return Status::OK();
    } else {
        return status;
    }
}

namespace {
void addRoleNameToObjectElement(mutablebson::Element object, const RoleName& role) {
    fassert(17153, object.appendString(AuthorizationManager::ROLE_NAME_FIELD_NAME, role.getRole()));
    fassert(17154, object.appendString(AuthorizationManager::ROLE_DB_FIELD_NAME, role.getDB()));
}

void addRoleNameObjectsToArrayElement(mutablebson::Element array, RoleNameIterator roles) {
    for (; roles.more(); roles.next()) {
        mutablebson::Element roleElement = array.getDocument().makeElementObject("");
        addRoleNameToObjectElement(roleElement, roles.get());
        fassert(17155, array.pushBack(roleElement));
    }
}

void addPrivilegeObjectsOrWarningsToArrayElement(mutablebson::Element privilegesElement,
                                                 mutablebson::Element warningsElement,
                                                 const PrivilegeVector& privileges) {
    std::string errmsg;
    for (size_t i = 0; i < privileges.size(); ++i) {
        ParsedPrivilege pp;
        if (ParsedPrivilege::privilegeToParsedPrivilege(privileges[i], &pp, &errmsg)) {
            fassert(17156, privilegesElement.appendObject("", pp.toBSON()));
        } else {
            fassert(17157,
                    warningsElement.appendString(
                        "",
                        std::string(mongoutils::str::stream()
                                    << "Skipped privileges on resource "
                                    << privileges[i].getResourcePattern().toString()
                                    << ". Reason: "
                                    << errmsg)));
        }
    }
}

void addAuthenticationRestrictionObjectsToArrayElement(
    mutablebson::Element restrictionsElement,
    const std::vector<SharedRestrictionDocument>& restrictions) {
    for (const auto& r : restrictions) {
        fassert(40560, restrictionsElement.appendArray("", r->toBSON()));
    }
}
}  // namespace

bool AuthzManagerExternalStateLocal::hasAnyPrivilegeDocuments(OperationContext* opCtx) {
    BSONObj userBSONObj;
    Status statusFindUsers =
        findOne(opCtx, AuthorizationManager::usersCollectionNamespace, BSONObj(), &userBSONObj);

    // If we were unable to complete the query,
    // it's best to assume that there _are_ privilege documents.
    if (statusFindUsers != ErrorCodes::NoMatchingDocument) {
        return true;
    }
    Status statusFindRoles =
        findOne(opCtx, AuthorizationManager::rolesCollectionNamespace, BSONObj(), &userBSONObj);
    return statusFindRoles != ErrorCodes::NoMatchingDocument;
}

Status AuthzManagerExternalStateLocal::getUserDescription(OperationContext* opCtx,
                                                          const UserName& userName,
                                                          BSONObj* result) {
    Status status = Status::OK();

    if (!shouldUseRolesFromConnection(opCtx, userName)) {
        status = _getUserDocument(opCtx, userName, result);
        if (!status.isOK())
            return status;
    } else {
        // We are able to artifically construct the external user from the request
        BSONArrayBuilder userRoles;
        auto& sslPeerInfo = SSLPeerInfo::forSession(opCtx->getClient()->session());
        for (const RoleName& role : sslPeerInfo.roles) {
            userRoles << BSON("role" << role.getRole() << "db" << role.getDB());
        }
        *result = BSON("_id" << userName.getUser() << "user" << userName.getUser() << "db"
                             << userName.getDB()
                             << "credentials"
                             << BSON("external" << true)
                             << "roles"
                             << userRoles.arr());
    }

    BSONElement directRolesElement;
    status = bsonExtractTypedField(*result, "roles", Array, &directRolesElement);
    if (!status.isOK())
        return status;
    std::vector<RoleName> directRoles;
    status =
        V2UserDocumentParser::parseRoleVector(BSONArray(directRolesElement.Obj()), &directRoles);
    if (!status.isOK())
        return status;

    mutablebson::Document resultDoc(*result, mutablebson::Document::kInPlaceDisabled);
    resolveUserRoles(&resultDoc, directRoles);
    *result = resultDoc.getObject();

    const auto isNonEmptyArray = [](const BSONObj& doc, StringData element) {
        const auto& e = doc[element];
        return !e.eoo() && (e.type() == Array) && !e.Obj().isEmpty();
    };

    if ((isNonEmptyArray(*result, "authenticationRestrictions") ||
         isNonEmptyArray(*result, "inheritedAuthenticationRestrictions")) &&
        serverGlobalParams.featureCompatibility.version.load() <
            ServerGlobalParams::FeatureCompatibility::Version::k36) {
        // Mongos isn't able to evaluate whether documents are valid under the current
        // featureCompatibilityVersion. We must make the decision before it sees them.
        return Status(ErrorCodes::UnsupportedFormat,
                      "'authenticationRestrictions' requires 3.6 feature compatibility version");
    }

    return Status::OK();
}

void AuthzManagerExternalStateLocal::resolveUserRoles(mutablebson::Document* userDoc,
                                                      const std::vector<RoleName>& directRoles) {
    unordered_set<RoleName> indirectRoles;
    PrivilegeVector allPrivileges;
    std::vector<SharedRestrictionDocument> allAuthenticationRestrictions;
    bool isRoleGraphConsistent = false;

    {
        stdx::lock_guard<stdx::mutex> lk(_roleGraphMutex);
        isRoleGraphConsistent = _roleGraphState == roleGraphStateConsistent;
        for (const auto& role : directRoles) {
            indirectRoles.insert(role);
            if (isRoleGraphConsistent) {
                for (RoleNameIterator subordinates = _roleGraph.getIndirectSubordinates(role);
                     subordinates.more();
                     subordinates.next()) {
                    indirectRoles.insert(subordinates.get());
                }
            }

            const auto& currentPrivileges = isRoleGraphConsistent
                ? _roleGraph.getAllPrivileges(role)
                : _roleGraph.getDirectPrivileges(role);
            for (const auto& priv : currentPrivileges) {
                Privilege::addPrivilegeToPrivilegeVector(&allPrivileges, priv);
            }

            if (isRoleGraphConsistent) {
                const auto& currentAuthenticationRestrictions =
                    _roleGraph.getAllAuthenticationRestrictions(role);
                allAuthenticationRestrictions.insert(allAuthenticationRestrictions.end(),
                                                     currentAuthenticationRestrictions.begin(),
                                                     currentAuthenticationRestrictions.end());
            } else {
                const auto& dar = _roleGraph.getDirectAuthenticationRestrictions(role);
                if (dar.get()) {
                    allAuthenticationRestrictions.push_back(dar);
                }
            }
        }
    }

    auto warningsElement = userDoc->makeElementArray("warnings");

    auto inheritedRolesElement = userDoc->makeElementArray("inheritedRoles");
    fassert(17159, userDoc->root().pushBack(inheritedRolesElement));
    addRoleNameObjectsToArrayElement(inheritedRolesElement,
                                     makeRoleNameIteratorForContainer(indirectRoles));

    auto privilegesElement = userDoc->makeElementArray("inheritedPrivileges");
    fassert(17158, userDoc->root().pushBack(privilegesElement));
    addPrivilegeObjectsOrWarningsToArrayElement(privilegesElement, warningsElement, allPrivileges);

    auto authenticationRestrictionsElement =
        userDoc->makeElementArray("inheritedAuthenticationRestrictions");
    fassert(40558, userDoc->root().pushBack(authenticationRestrictionsElement));
    addAuthenticationRestrictionObjectsToArrayElement(authenticationRestrictionsElement,
                                                      allAuthenticationRestrictions);

    if (!isRoleGraphConsistent) {
        fassert(17160,
                warningsElement.appendString(
                    "", "Role graph inconsistent, only direct privileges available."));
    }

    if (warningsElement.hasChildren()) {
        fassert(17161, userDoc->root().pushBack(warningsElement));
    }
}

Status AuthzManagerExternalStateLocal::_getUserDocument(OperationContext* opCtx,
                                                        const UserName& userName,
                                                        BSONObj* userDoc) {
    Status status = findOne(opCtx,
                            AuthorizationManager::usersCollectionNamespace,
                            BSON(AuthorizationManager::USER_NAME_FIELD_NAME
                                 << userName.getUser()
                                 << AuthorizationManager::USER_DB_FIELD_NAME
                                 << userName.getDB()),
                            userDoc);

    if (status == ErrorCodes::NoMatchingDocument) {
        status =
            Status(ErrorCodes::UserNotFound,
                   mongoutils::str::stream() << "Could not find user " << userName.getFullName());
    } else if ((*userDoc)["authenticationRestrictions"] &&
               serverGlobalParams.featureCompatibility.version.load() <
                   ServerGlobalParams::FeatureCompatibility::Version::k36) {
        // Mongos isn't able to evaluate whether documents are valid under the current
        // featureCompatibilityVersion. We must make the decision before it sees them.
        status = Status(ErrorCodes::UnsupportedFormat,
                        "'authenticationRestrictions' requires 3.6 feature compatibility version");
    }
    return status;
}

Status AuthzManagerExternalStateLocal::getRoleDescription(
    OperationContext* opCtx,
    const RoleName& roleName,
    PrivilegeFormat showPrivileges,
    AuthenticationRestrictionsFormat showRestrictions,
    BSONObj* result) {
    if (showPrivileges == PrivilegeFormat::kShowAsUserFragment) {
        mutablebson::Document resultDoc;
        mutablebson::Element rolesElement = resultDoc.makeElementArray("roles");
        fassert(40273, resultDoc.root().pushBack(rolesElement));
        addRoleNameObjectsToArrayElement(
            rolesElement, makeRoleNameIteratorForContainer(std::vector<RoleName>{roleName}));
        resolveUserRoles(&resultDoc, {roleName});
        *result = resultDoc.getObject();
        return Status::OK();
    }
    stdx::lock_guard<stdx::mutex> lk(_roleGraphMutex);
    return _getRoleDescription_inlock(roleName, showPrivileges, showRestrictions, result);
}

Status AuthzManagerExternalStateLocal::getRolesDescription(
    OperationContext* opCtx,
    const std::vector<RoleName>& roles,
    PrivilegeFormat showPrivileges,
    AuthenticationRestrictionsFormat showRestrictions,
    BSONObj* result) {
    if (showPrivileges == PrivilegeFormat::kShowAsUserFragment) {
        mutablebson::Document resultDoc;
        mutablebson::Element rolesElement = resultDoc.makeElementArray("roles");
        fassert(40274, resultDoc.root().pushBack(rolesElement));
        addRoleNameObjectsToArrayElement(rolesElement, makeRoleNameIteratorForContainer(roles));
        resolveUserRoles(&resultDoc, roles);
        *result = resultDoc.getObject();
        return Status::OK();
    }

    stdx::lock_guard<stdx::mutex> lk(_roleGraphMutex);
    BSONArrayBuilder resultBuilder;
    for (const RoleName& role : roles) {
        BSONObj roleDoc;
        Status status =
            _getRoleDescription_inlock(role, showPrivileges, showRestrictions, &roleDoc);
        if (!status.isOK()) {
            if (status.code() == ErrorCodes::RoleNotFound) {
                continue;
            }
            return status;
        }
        resultBuilder << roleDoc;
    }
    *result = resultBuilder.arr();
    return Status::OK();
}

Status AuthzManagerExternalStateLocal::_getRoleDescription_inlock(
    const RoleName& roleName,
    PrivilegeFormat showPrivileges,
    AuthenticationRestrictionsFormat showRestrictions,
    BSONObj* result) {
    if (!_roleGraph.roleExists(roleName))
        return Status(ErrorCodes::RoleNotFound, "No role named " + roleName.toString());

    mutablebson::Document resultDoc;
    fassert(17162,
            resultDoc.root().appendString(AuthorizationManager::ROLE_NAME_FIELD_NAME,
                                          roleName.getRole()));
    fassert(
        17163,
        resultDoc.root().appendString(AuthorizationManager::ROLE_DB_FIELD_NAME, roleName.getDB()));
    fassert(17267, resultDoc.root().appendBool("isBuiltin", _roleGraph.isBuiltinRole(roleName)));

    auto warningsElement = resultDoc.makeElementArray("warnings");

    auto rolesElement = resultDoc.makeElementArray("roles");
    fassert(17164, resultDoc.root().pushBack(rolesElement));
    addRoleNameObjectsToArrayElement(rolesElement, _roleGraph.getDirectSubordinates(roleName));

    auto inheritedRolesElement = resultDoc.makeElementArray("inheritedRoles");
    fassert(17165, resultDoc.root().pushBack(inheritedRolesElement));

    auto privilegesElement = resultDoc.makeElementArray("privileges");
    if (showPrivileges == PrivilegeFormat::kShowSeparate) {
        fassert(17166, resultDoc.root().pushBack(privilegesElement));
    }

    if (showRestrictions == AuthenticationRestrictionsFormat::kShow) {
        auto authenticationRestrictionsElement =
            resultDoc.makeElementArray("authenticationRestrictions");
        fassert(40559, resultDoc.root().pushBack(authenticationRestrictionsElement));

        const auto& restrictions = _roleGraph.getDirectAuthenticationRestrictions(roleName);
        if (restrictions.get()) {
            fassert(40561,
                    authenticationRestrictionsElement.appendArray("", restrictions->toBSON()));
        }
    }

    if (_roleGraphState == roleGraphStateConsistent) {
        addRoleNameObjectsToArrayElement(inheritedRolesElement,
                                         _roleGraph.getIndirectSubordinates(roleName));

        if (showPrivileges == PrivilegeFormat::kShowSeparate) {
            auto inheritedPrivilegesElement = resultDoc.makeElementArray("inheritedPrivileges");
            addPrivilegeObjectsOrWarningsToArrayElement(
                privilegesElement, warningsElement, _roleGraph.getDirectPrivileges(roleName));

            addPrivilegeObjectsOrWarningsToArrayElement(
                inheritedPrivilegesElement, warningsElement, _roleGraph.getAllPrivileges(roleName));

            fassert(17323, resultDoc.root().pushBack(inheritedPrivilegesElement));
        }

        if (showRestrictions == AuthenticationRestrictionsFormat::kShow) {
            auto inheritedAuthenticationRestrictionsElement =
                resultDoc.makeElementArray("inheritedAuthenticationRestrictions");
            fassert(40563, resultDoc.root().pushBack(inheritedAuthenticationRestrictionsElement));

            for (const auto& restrictions : _roleGraph.getAllAuthenticationRestrictions(roleName)) {
                fassert(40562,
                        inheritedAuthenticationRestrictionsElement.appendArray(
                            "", restrictions->toBSON()));
            }
        }
    } else if (showPrivileges == PrivilegeFormat::kShowSeparate) {
        addPrivilegeObjectsOrWarningsToArrayElement(
            privilegesElement, warningsElement, _roleGraph.getDirectPrivileges(roleName));
        fassert(40557,
                warningsElement.appendString("",
                                             "Role graph state inconsistent; only direct "
                                             "privileges and restrictions available."));
    }

    if (warningsElement.hasChildren()) {
        fassert(17167, resultDoc.root().pushBack(warningsElement));
    }
    *result = resultDoc.getObject();
    return Status::OK();
}

Status AuthzManagerExternalStateLocal::getRoleDescriptionsForDB(
    OperationContext* opCtx,
    const std::string& dbname,
    PrivilegeFormat showPrivileges,
    AuthenticationRestrictionsFormat showRestrictions,
    bool showBuiltinRoles,
    vector<BSONObj>* result) {
    if (showPrivileges == PrivilegeFormat::kShowAsUserFragment) {
        return Status(ErrorCodes::IllegalOperation,
                      "Cannot get user fragment for all roles in a database");
    }

    stdx::lock_guard<stdx::mutex> lk(_roleGraphMutex);
    for (RoleNameIterator it = _roleGraph.getRolesForDatabase(dbname); it.more(); it.next()) {
        if (!showBuiltinRoles && _roleGraph.isBuiltinRole(it.get())) {
            continue;
        }
        BSONObj roleDoc;
        Status status =
            _getRoleDescription_inlock(it.get(), showPrivileges, showRestrictions, &roleDoc);
        if (!status.isOK()) {
            return status;
        }
        result->push_back(roleDoc);
    }
    return Status::OK();
}

namespace {

/**
 * Adds the role described in "doc" to "roleGraph".  If the role cannot be added, due to
 * some error in "doc", logs a warning.
 */
void addRoleFromDocumentOrWarn(RoleGraph* roleGraph, const BSONObj& doc) {
    Status status = roleGraph->addRoleFromDocument(doc);
    if (!status.isOK()) {
        warning() << "Skipping invalid admin.system.roles document while calculating privileges"
                     " for user-defined roles:  "
                  << redact(status) << "; document " << redact(doc);
    }
}


}  // namespace

Status AuthzManagerExternalStateLocal::_initializeRoleGraph(OperationContext* opCtx) {
    stdx::lock_guard<stdx::mutex> lkInitialzeRoleGraph(_roleGraphMutex);

    _roleGraphState = roleGraphStateInitial;
    _roleGraph = RoleGraph();

    RoleGraph newRoleGraph;
    Status status =
        query(opCtx,
              AuthorizationManager::rolesCollectionNamespace,
              BSONObj(),
              BSONObj(),
              stdx::bind(addRoleFromDocumentOrWarn, &newRoleGraph, stdx::placeholders::_1));
    if (!status.isOK())
        return status;

    status = newRoleGraph.recomputePrivilegeData();

    RoleGraphState newState;
    if (status == ErrorCodes::GraphContainsCycle) {
        error() << "Inconsistent role graph during authorization manager initialization.  Only "
                   "direct privileges available. "
                << redact(status);
        newState = roleGraphStateHasCycle;
        status = Status::OK();
    } else if (status.isOK()) {
        newState = roleGraphStateConsistent;
    } else {
        newState = roleGraphStateInitial;
    }

    if (status.isOK()) {
        _roleGraph = std::move(newRoleGraph);
        _roleGraphState = std::move(newState);
    }
    return status;
}

class AuthzManagerExternalStateLocal::AuthzManagerLogOpHandler : public RecoveryUnit::Change {
public:
    // None of the parameters below (except opCtx and externalState) need to live longer than the
    // instantiations of this class
    AuthzManagerLogOpHandler(OperationContext* opCtx,
                             AuthzManagerExternalStateLocal* externalState,
                             const char* op,
                             const NamespaceString& nss,
                             const BSONObj& o,
                             const BSONObj* o2)
        : _opCtx(opCtx),
          _externalState(externalState),
          _op(op),
          _nss(nss),
          _o(o.getOwned()),

          _isO2Set(o2 ? true : false),
          _o2(_isO2Set ? o2->getOwned() : BSONObj()) {}

    virtual void commit() {
        stdx::lock_guard<stdx::mutex> lk(_externalState->_roleGraphMutex);
        Status status = _externalState->_roleGraph.handleLogOp(
            _opCtx, _op.c_str(), _nss, _o, _isO2Set ? &_o2 : NULL);

        if (status == ErrorCodes::OplogOperationUnsupported) {
            _externalState->_roleGraph = RoleGraph();
            _externalState->_roleGraphState = _externalState->roleGraphStateInitial;
            BSONObjBuilder oplogEntryBuilder;
            oplogEntryBuilder << "op" << _op << "ns" << _nss.ns() << "o" << _o;
            if (_isO2Set)
                oplogEntryBuilder << "o2" << _o2;
            error() << "Unsupported modification to roles collection in oplog; "
                       "restart this process to reenable user-defined roles; "
                    << redact(status) << "; Oplog entry: " << redact(oplogEntryBuilder.done());
        } else if (!status.isOK()) {
            warning() << "Skipping bad update to roles collection in oplog. " << redact(status)
                      << " Oplog entry: " << redact(_op);
        }
        status = _externalState->_roleGraph.recomputePrivilegeData();
        if (status == ErrorCodes::GraphContainsCycle) {
            _externalState->_roleGraphState = _externalState->roleGraphStateHasCycle;
            error() << "Inconsistent role graph during authorization manager initialization.  "
                       "Only direct privileges available. "
                    << redact(status) << " after applying oplog entry " << redact(_op);
        } else {
            fassert(17183, status);
            _externalState->_roleGraphState = _externalState->roleGraphStateConsistent;
        }
    }

    virtual void rollback() {}

private:
    OperationContext* _opCtx;
    AuthzManagerExternalStateLocal* _externalState;
    const std::string _op;
    const NamespaceString _nss;
    const BSONObj _o;

    const bool _isO2Set;
    const BSONObj _o2;
};

void AuthzManagerExternalStateLocal::logOp(OperationContext* opCtx,
                                           const char* op,
                                           const NamespaceString& nss,
                                           const BSONObj& o,
                                           const BSONObj* o2) {
    if (nss == AuthorizationManager::rolesCollectionNamespace ||
        nss == AuthorizationManager::adminCommandNamespace) {
        opCtx->recoveryUnit()->registerChange(
            new AuthzManagerLogOpHandler(opCtx, this, op, nss, o, o2));
    }
}

}  // namespace mongo
