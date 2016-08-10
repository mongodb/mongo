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
#include "mongo/db/auth/user_document_parser.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using std::vector;

Status AuthzManagerExternalStateLocal::initialize(OperationContext* txn) {
    Status status = _initializeRoleGraph(txn);
    if (!status.isOK()) {
        if (status == ErrorCodes::GraphContainsCycle) {
            error() << "Cycle detected in admin.system.roles; role inheritance disabled. "
                       "Remove the listed cycle and any others to re-enable role inheritance. "
                    << status.reason();
        } else {
            error() << "Could not generate role graph from admin.system.roles; "
                       "only system roles available: "
                    << status;
        }
    }

    return Status::OK();
}

Status AuthzManagerExternalStateLocal::getStoredAuthorizationVersion(OperationContext* txn,
                                                                     int* outVersion) {
    BSONObj versionDoc;
    Status status = findOne(txn,
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
}  // namespace

bool AuthzManagerExternalStateLocal::hasAnyPrivilegeDocuments(OperationContext* txn) {
    BSONObj userBSONObj;
    Status statusFindUsers =
        findOne(txn, AuthorizationManager::usersCollectionNamespace, BSONObj(), &userBSONObj);

    // If we were unable to complete the query,
    // it's best to assume that there _are_ privilege documents.
    if (statusFindUsers != ErrorCodes::NoMatchingDocument) {
        return true;
    }
    Status statusFindRoles =
        findOne(txn, AuthorizationManager::rolesCollectionNamespace, BSONObj(), &userBSONObj);
    return statusFindRoles != ErrorCodes::NoMatchingDocument;
}

Status AuthzManagerExternalStateLocal::getUserDescription(OperationContext* txn,
                                                          const UserName& userName,
                                                          BSONObj* result) {
    Status status = _getUserDocument(txn, userName, result);
    if (!status.isOK())
        return status;

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

    return Status::OK();
}

void AuthzManagerExternalStateLocal::resolveUserRoles(mutablebson::Document* userDoc,
                                                      const std::vector<RoleName>& directRoles) {
    unordered_set<RoleName> indirectRoles;
    PrivilegeVector allPrivileges;
    bool isRoleGraphInconsistent;
    {
        stdx::lock_guard<stdx::mutex> lk(_roleGraphMutex);
        isRoleGraphInconsistent = _roleGraphState == roleGraphStateConsistent;
        for (size_t i = 0; i < directRoles.size(); ++i) {
            const RoleName& role(directRoles[i]);
            indirectRoles.insert(role);
            if (isRoleGraphInconsistent) {
                for (RoleNameIterator subordinates = _roleGraph.getIndirectSubordinates(role);
                     subordinates.more();
                     subordinates.next()) {
                    indirectRoles.insert(subordinates.get());
                }
            }
            const PrivilegeVector& rolePrivileges(isRoleGraphInconsistent
                                                      ? _roleGraph.getAllPrivileges(role)
                                                      : _roleGraph.getDirectPrivileges(role));
            for (PrivilegeVector::const_iterator priv = rolePrivileges.begin(),
                                                 end = rolePrivileges.end();
                 priv != end;
                 ++priv) {
                Privilege::addPrivilegeToPrivilegeVector(&allPrivileges, *priv);
            }
        }
    }

    mutablebson::Element inheritedRolesElement = userDoc->makeElementArray("inheritedRoles");
    mutablebson::Element privilegesElement = userDoc->makeElementArray("inheritedPrivileges");
    mutablebson::Element warningsElement = userDoc->makeElementArray("warnings");
    fassert(17159, userDoc->root().pushBack(inheritedRolesElement));
    fassert(17158, userDoc->root().pushBack(privilegesElement));
    if (!isRoleGraphInconsistent) {
        fassert(17160,
                warningsElement.appendString(
                    "", "Role graph inconsistent, only direct privileges available."));
    }
    addRoleNameObjectsToArrayElement(inheritedRolesElement,
                                     makeRoleNameIteratorForContainer(indirectRoles));
    addPrivilegeObjectsOrWarningsToArrayElement(privilegesElement, warningsElement, allPrivileges);
    if (warningsElement.hasChildren()) {
        fassert(17161, userDoc->root().pushBack(warningsElement));
    }
}

Status AuthzManagerExternalStateLocal::_getUserDocument(OperationContext* txn,
                                                        const UserName& userName,
                                                        BSONObj* userDoc) {
    Status status = findOne(txn,
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
    }
    return status;
}

Status AuthzManagerExternalStateLocal::getRoleDescription(OperationContext* txn,
                                                          const RoleName& roleName,
                                                          bool showPrivileges,
                                                          BSONObj* result) {
    stdx::lock_guard<stdx::mutex> lk(_roleGraphMutex);
    return _getRoleDescription_inlock(roleName, showPrivileges, result);
}

Status AuthzManagerExternalStateLocal::_getRoleDescription_inlock(const RoleName& roleName,
                                                                  bool showPrivileges,
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
    mutablebson::Element rolesElement = resultDoc.makeElementArray("roles");
    fassert(17164, resultDoc.root().pushBack(rolesElement));
    mutablebson::Element inheritedRolesElement = resultDoc.makeElementArray("inheritedRoles");
    fassert(17165, resultDoc.root().pushBack(inheritedRolesElement));
    mutablebson::Element privilegesElement = resultDoc.makeElementArray("privileges");
    mutablebson::Element inheritedPrivilegesElement =
        resultDoc.makeElementArray("inheritedPrivileges");
    if (showPrivileges) {
        fassert(17166, resultDoc.root().pushBack(privilegesElement));
    }
    mutablebson::Element warningsElement = resultDoc.makeElementArray("warnings");

    addRoleNameObjectsToArrayElement(rolesElement, _roleGraph.getDirectSubordinates(roleName));
    if (_roleGraphState == roleGraphStateConsistent) {
        addRoleNameObjectsToArrayElement(inheritedRolesElement,
                                         _roleGraph.getIndirectSubordinates(roleName));
        if (showPrivileges) {
            addPrivilegeObjectsOrWarningsToArrayElement(
                privilegesElement, warningsElement, _roleGraph.getDirectPrivileges(roleName));

            addPrivilegeObjectsOrWarningsToArrayElement(
                inheritedPrivilegesElement, warningsElement, _roleGraph.getAllPrivileges(roleName));

            fassert(17323, resultDoc.root().pushBack(inheritedPrivilegesElement));
        }
    } else if (showPrivileges) {
        warningsElement.appendString(
            "", "Role graph state inconsistent; only direct privileges available.");
        addPrivilegeObjectsOrWarningsToArrayElement(
            privilegesElement, warningsElement, _roleGraph.getDirectPrivileges(roleName));
    }
    if (warningsElement.hasChildren()) {
        fassert(17167, resultDoc.root().pushBack(warningsElement));
    }
    *result = resultDoc.getObject();
    return Status::OK();
}

Status AuthzManagerExternalStateLocal::getRoleDescriptionsForDB(OperationContext* txn,
                                                                const std::string dbname,
                                                                bool showPrivileges,
                                                                bool showBuiltinRoles,
                                                                vector<BSONObj>* result) {
    stdx::lock_guard<stdx::mutex> lk(_roleGraphMutex);

    for (RoleNameIterator it = _roleGraph.getRolesForDatabase(dbname); it.more(); it.next()) {
        if (!showBuiltinRoles && _roleGraph.isBuiltinRole(it.get())) {
            continue;
        }
        BSONObj roleDoc;
        Status status = _getRoleDescription_inlock(it.get(), showPrivileges, &roleDoc);
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
                  << status << "; document " << doc;
    }
}


}  // namespace

Status AuthzManagerExternalStateLocal::_initializeRoleGraph(OperationContext* txn) {
    stdx::lock_guard<stdx::mutex> lkInitialzeRoleGraph(_roleGraphMutex);

    _roleGraphState = roleGraphStateInitial;
    _roleGraph = RoleGraph();

    RoleGraph newRoleGraph;
    Status status =
        query(txn,
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
                << status.reason();
        newState = roleGraphStateHasCycle;
        status = Status::OK();
    } else if (status.isOK()) {
        newState = roleGraphStateConsistent;
    } else {
        newState = roleGraphStateInitial;
    }

    if (status.isOK()) {
        _roleGraph.swap(newRoleGraph);
        _roleGraphState = newState;
    }
    return status;
}

class AuthzManagerExternalStateLocal::AuthzManagerLogOpHandler : public RecoveryUnit::Change {
public:
    // None of the parameters below (except txn and externalState) need to live longer than the
    // instantiations of this class
    AuthzManagerLogOpHandler(OperationContext* txn,
                             AuthzManagerExternalStateLocal* externalState,
                             const char* op,
                             const char* ns,
                             const BSONObj& o,
                             const BSONObj* o2)
        : _txn(txn),
          _externalState(externalState),
          _op(op),
          _ns(ns),
          _o(o.getOwned()),

          _isO2Set(o2 ? true : false),
          _o2(_isO2Set ? o2->getOwned() : BSONObj()) {}

    virtual void commit() {
        stdx::lock_guard<stdx::mutex> lk(_externalState->_roleGraphMutex);
        Status status = _externalState->_roleGraph.handleLogOp(
            _txn, _op.c_str(), NamespaceString(_ns.c_str()), _o, _isO2Set ? &_o2 : NULL);

        if (status == ErrorCodes::OplogOperationUnsupported) {
            _externalState->_roleGraph = RoleGraph();
            _externalState->_roleGraphState = _externalState->roleGraphStateInitial;
            BSONObjBuilder oplogEntryBuilder;
            oplogEntryBuilder << "op" << _op << "ns" << _ns << "o" << _o;
            if (_isO2Set)
                oplogEntryBuilder << "o2" << _o2;
            error() << "Unsupported modification to roles collection in oplog; "
                       "restart this process to reenable user-defined roles; "
                    << status.reason() << "; Oplog entry: " << oplogEntryBuilder.done();
        } else if (!status.isOK()) {
            warning() << "Skipping bad update to roles collection in oplog. " << status
                      << " Oplog entry: " << _op;
        }
        status = _externalState->_roleGraph.recomputePrivilegeData();
        if (status == ErrorCodes::GraphContainsCycle) {
            _externalState->_roleGraphState = _externalState->roleGraphStateHasCycle;
            error() << "Inconsistent role graph during authorization manager initialization.  "
                       "Only direct privileges available. "
                    << status.reason() << " after applying oplog entry " << _op;
        } else {
            fassert(17183, status);
            _externalState->_roleGraphState = _externalState->roleGraphStateConsistent;
        }
    }

    virtual void rollback() {}

private:
    OperationContext* _txn;
    AuthzManagerExternalStateLocal* _externalState;
    const std::string _op;
    const std::string _ns;
    const BSONObj _o;

    const bool _isO2Set;
    const BSONObj _o2;
};

void AuthzManagerExternalStateLocal::logOp(
    OperationContext* txn, const char* op, const char* ns, const BSONObj& o, const BSONObj* o2) {
    if (ns == AuthorizationManager::rolesCollectionNamespace.ns() ||
        ns == AuthorizationManager::adminCommandNamespace.ns()) {
        txn->recoveryUnit()->registerChange(new AuthzManagerLogOpHandler(txn, this, op, ns, o, o2));
    }
}

}  // namespace mongo
