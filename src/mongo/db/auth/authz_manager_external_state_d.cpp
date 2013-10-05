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

#include "mongo/db/auth/authz_manager_external_state_d.h"

#include <boost/thread/mutex.hpp>
#include <string>

#include "mongo/base/status.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/bson/mutable/element.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/user_document_parser.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/client.h"
#include "mongo/db/d_concurrency.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/instance.h"
#include "mongo/db/jsobj.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    AuthzManagerExternalStateMongod::AuthzManagerExternalStateMongod() :
        _roleGraphState(roleGraphStateInitial) {}

    AuthzManagerExternalStateMongod::~AuthzManagerExternalStateMongod() {}

    Status AuthzManagerExternalStateMongod::initialize() {
        Status status = _initializeRoleGraph();
        if (!status.isOK()) {
            if (status == ErrorCodes::GraphContainsCycle) {
                error() << "Cycle detected in admin.system.roles; role inheritance disabled. "
                    "TODO EXPLAIN TO REMEDY. " << status.reason();
            }
            else {
                error() << "Could not generate role graph from admin.system.roles; "
                    "only system roles available. TODO EXPLAIN REMEDY. " << status;
            }
        }

        return Status::OK();
    }

namespace {
    const Status userNotFoundStatus(ErrorCodes::UserNotFound, "User not found");

    void addRoleNameToObjectElement(mutablebson::Element object, const RoleName& role) {
        fassert(17153, object.appendString(AuthorizationManager::ROLE_NAME_FIELD_NAME, role.getRole()));
        fassert(17154, object.appendString(AuthorizationManager::ROLE_SOURCE_FIELD_NAME, role.getDB()));
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
                        std::string(mongoutils::str::stream() <<
                                    "Skipped privileges on resource " <<
                                    privileges[i].getResourcePattern().toString() <<
                                    ". Reason: " << errmsg)));
            }
        }
    }
}  // namespace

    Status AuthzManagerExternalStateMongod::getUserDescription(const UserName& userName,
                                                               BSONObj* result) {

        BSONObj userDoc;
        Status status = _findUser(
                "admin.system.users",
                BSON(AuthorizationManager::USER_NAME_FIELD_NAME << userName.getUser() <<
                     AuthorizationManager::USER_SOURCE_FIELD_NAME << userName.getDB()),
                &userDoc);
        if (!status.isOK())
            return status;

        BSONElement directRolesElement;
        status = bsonExtractTypedField(userDoc, "roles", Array, &directRolesElement);
        if (!status.isOK())
            return status;
        std::vector<User::RoleData> directRoles;
        status = V2UserDocumentParser::parseRoleVector(BSONArray(directRolesElement.Obj()),
                                                      &directRoles);
        if (!status.isOK())
            return status;

        unordered_set<RoleName> indirectRoles;
        PrivilegeVector allPrivileges;
        bool isRoleGraphInconsistent;
        {
            boost::lock_guard<boost::mutex> lk(_roleGraphMutex);
            isRoleGraphInconsistent = _roleGraphState == roleGraphStateConsistent;
            for (size_t i = 0; i < directRoles.size(); ++i) {
                const User::RoleData& role(directRoles[i]);
                if (!role.hasRole)
                    continue;
                indirectRoles.insert(role.name);
                if (isRoleGraphInconsistent) {
                    for (RoleNameIterator subordinates = _roleGraph.getIndirectSubordinates(
                                 role.name);
                         subordinates.more();
                         subordinates.next()) {

                        indirectRoles.insert(subordinates.get());
                    }
                }
                const PrivilegeVector& rolePrivileges(
                        isRoleGraphInconsistent ?
                        _roleGraph.getAllPrivileges(role.name) :
                        _roleGraph.getDirectPrivileges(role.name));
                for (PrivilegeVector::const_iterator priv = rolePrivileges.begin(),
                         end = rolePrivileges.end();
                     priv != end;
                     ++priv) {

                    Privilege::addPrivilegeToPrivilegeVector(&allPrivileges, *priv);
                }
            }
        }

        mutablebson::Document resultDoc(userDoc, mutablebson::Document::kInPlaceDisabled);
        mutablebson::Element indirectRolesElement = resultDoc.makeElementArray("indirectRoles");
        mutablebson::Element privilegesElement = resultDoc.makeElementArray("privileges");
        mutablebson::Element warningsElement = resultDoc.makeElementArray("warnings");
        fassert(17158, resultDoc.root().pushBack(privilegesElement));
        fassert(17159, resultDoc.root().pushBack(indirectRolesElement));
        if (!isRoleGraphInconsistent) {
            fassert(17160, warningsElement.appendString(
                            "", "Role graph inconsistent, only direct privileges available."));
        }
        addRoleNameObjectsToArrayElement(indirectRolesElement,
                                         makeRoleNameIteratorForContainer(indirectRoles));
        addPrivilegeObjectsOrWarningsToArrayElement(
                privilegesElement, warningsElement, allPrivileges);
        if (warningsElement.hasChildren()) {
            fassert(17161, resultDoc.root().pushBack(warningsElement));
        }
        *result = resultDoc.getObject();
        return Status::OK();
    }

    Status AuthzManagerExternalStateMongod::getRoleDescription(const RoleName& roleName,
                                                               BSONObj* result) {
        boost::lock_guard<boost::mutex> lk(_roleGraphMutex);
        if (!_roleGraph.roleExists(roleName))
            return Status(ErrorCodes::RoleNotFound, "No role named " + roleName.toString());

        mutablebson::Document resultDoc;
        fassert(17162, resultDoc.root().appendString(
                        AuthorizationManager::ROLE_NAME_FIELD_NAME, roleName.getRole()));
        fassert(17163, resultDoc.root().appendString(
                        AuthorizationManager::ROLE_SOURCE_FIELD_NAME, roleName.getDB()));
        mutablebson::Element rolesElement = resultDoc.makeElementArray("roles");
        fassert(17164, resultDoc.root().pushBack(rolesElement));
        mutablebson::Element indirectRolesElement = resultDoc.makeElementArray("indirectRoles");
        fassert(17165, resultDoc.root().pushBack(indirectRolesElement));
        mutablebson::Element privilegesElement = resultDoc.makeElementArray("privileges");
        fassert(17166, resultDoc.root().pushBack(privilegesElement));
        mutablebson::Element warningsElement = resultDoc.makeElementArray("warnings");

        addRoleNameObjectsToArrayElement(rolesElement, _roleGraph.getDirectSubordinates(roleName));
        if (_roleGraphState == roleGraphStateConsistent) {
            addRoleNameObjectsToArrayElement(
                    indirectRolesElement, _roleGraph.getIndirectSubordinates(roleName));
            addPrivilegeObjectsOrWarningsToArrayElement(
                    privilegesElement, warningsElement, _roleGraph.getAllPrivileges(roleName));
        }
        else {
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

    Status AuthzManagerExternalStateMongod::_findUser(const string& usersNamespace,
                                                      const BSONObj& query,
                                                      BSONObj* result) {
        Client::GodScope gs;
        Client::ReadContext ctx(usersNamespace);

        if (!Helpers::findOne(usersNamespace, query, *result)) {
            return userNotFoundStatus;
        }
        return Status::OK();
    }

    Status AuthzManagerExternalStateMongod::query(
            const NamespaceString& collectionName,
            const BSONObj& query,
            const boost::function<void(const BSONObj&)>& resultProcessor) {
        try {
            DBDirectClient client;
            Client::GodScope gs;
            client.query(resultProcessor, collectionName.ns(), query);
            return Status::OK();
        } catch (const DBException& e) {
            return e.toStatus();
        }
    }

    Status AuthzManagerExternalStateMongod::getAllDatabaseNames(
            std::vector<std::string>* dbnames) {
        Lock::GlobalRead lk;
        getDatabaseNames(*dbnames);
        return Status::OK();
    }

    Status AuthzManagerExternalStateMongod::getAllV1PrivilegeDocsForDB(
            const std::string& dbname, std::vector<BSONObj>* privDocs) {
        std::string usersNamespace = dbname + ".system.users";

        Client::GodScope gs;
        Client::ReadContext ctx(usersNamespace);

        *privDocs = Helpers::findAll(usersNamespace, BSONObj());
        return Status::OK();
    }

    Status AuthzManagerExternalStateMongod::findOne(
            const NamespaceString& collectionName,
            const BSONObj& query,
            BSONObj* result) {
        fassertFailed(17091);
    }

    Status AuthzManagerExternalStateMongod::insert(
            const NamespaceString& collectionName,
            const BSONObj& document,
            const BSONObj& writeConcern) {
        try {
            DBDirectClient client;
            {
                Client::GodScope gs;
                // TODO(spencer): Once we're no longer fully rebuilding the user cache on every
                // change to user data we should remove the global lock and uncomment the
                // WriteContext below
                Lock::GlobalWrite w;
                // Client::WriteContext ctx(userNS);
                client.insert(collectionName, document);
            }

            // Handle write concern
            BSONObjBuilder gleBuilder;
            gleBuilder.append("getLastError", 1);
            gleBuilder.appendElements(writeConcern);
            BSONObj res;
            client.runCommand("admin", gleBuilder.done(), res);
            string errstr = client.getLastErrorString(res);
            if (errstr.empty()) {
                return Status::OK();
            }
            if (res.hasField("code") && res["code"].Int() == ASSERT_ID_DUPKEY) {
                return Status(ErrorCodes::DuplicateKey, errstr);
            }
            return Status(ErrorCodes::UnknownError, errstr);
        } catch (const DBException& e) {
            return e.toStatus();
        }
    }

    Status AuthzManagerExternalStateMongod::update(const NamespaceString& collectionName,
                                                   const BSONObj& query,
                                                   const BSONObj& updatePattern,
                                                   bool upsert,
                                                   bool multi,
                                                   const BSONObj& writeConcern,
                                                   int* numUpdated) {
        try {
            DBDirectClient client;
            {
                Client::GodScope gs;
                // TODO(spencer): Once we're no longer fully rebuilding the user cache on every
                // change to user data we should remove the global lock and uncomment the
                // WriteContext below
                Lock::GlobalWrite w;
                // Client::WriteContext ctx(userNS);
                client.update(collectionName, query, updatePattern, upsert, multi);
            }

            // Handle write concern
            BSONObjBuilder gleBuilder;
            gleBuilder.append("getLastError", 1);
            gleBuilder.appendElements(writeConcern);
            BSONObj res;
            client.runCommand("admin", gleBuilder.done(), res);
            string err = client.getLastErrorString(res);
            if (!err.empty()) {
                return Status(ErrorCodes::UnknownError, err);
            }

            *numUpdated = res["n"].numberInt();
            return Status::OK();
        } catch (const DBException& e) {
            return e.toStatus();
        }
    }

    Status AuthzManagerExternalStateMongod::remove(
            const NamespaceString& collectionName,
            const BSONObj& query,
            const BSONObj& writeConcern,
            int* numRemoved) {
        try {
            DBDirectClient client;
            {
                Client::GodScope gs;
                // TODO(spencer): Once we're no longer fully rebuilding the user cache on every
                // change to user data we should remove the global lock and uncomment the
                // WriteContext below
                Lock::GlobalWrite w;
                // Client::WriteContext ctx(userNS);
                client.remove(collectionName, query);
            }

            // Handle write concern
            BSONObjBuilder gleBuilder;
            gleBuilder.append("getLastError", 1);
            gleBuilder.appendElements(writeConcern);
            BSONObj res;
            client.runCommand("admin", gleBuilder.done(), res);
            string errstr = client.getLastErrorString(res);
            if (!errstr.empty()) {
                return Status(ErrorCodes::UnknownError, errstr);
            }

            *numRemoved = res["n"].numberInt();
            return Status::OK();
        } catch (const DBException& e) {
            return e.toStatus();
        }
    }

    Status AuthzManagerExternalStateMongod::createIndex(
            const NamespaceString& collectionName,
            const BSONObj& pattern,
            bool unique,
            const BSONObj& writeConcern) {
        fassertFailed(17095);
    }

    Status AuthzManagerExternalStateMongod::dropCollection(
            const NamespaceString& collectionName,
            const BSONObj& writeConcern) {
        fassertFailed(17096);
    }

    Status AuthzManagerExternalStateMongod::renameCollection(
            const NamespaceString& oldName,
            const NamespaceString& newName,
            const BSONObj& writeConcern) {
        fassertFailed(17097);
    }

    Status AuthzManagerExternalStateMongod::copyCollection(
            const NamespaceString& fromName,
            const NamespaceString& toName,
            const BSONObj& writeConcern) {
        fassertFailed(17098);
    }

    bool AuthzManagerExternalStateMongod::tryAcquireAuthzUpdateLock(const StringData& why) {
        log() << "Attempting to lock user data for: " << why << endl;
        return _authzDataUpdateLock.try_lock();
    }

    void AuthzManagerExternalStateMongod::releaseAuthzUpdateLock() {
        return _authzDataUpdateLock.unlock();
    }

namespace {

    /**
     * Adds the role described in "doc" to "roleGraph".  If the role cannot be added, due to
     * some error in "doc", logs a warning.
     */
    void addRoleFromDocumentOrWarn(RoleGraph* roleGraph, const BSONObj& doc) {
        Status status = roleGraph->addRoleFromDocument(doc);
        if (!status.isOK()) {
            warning() << "Skipping invalid role document.  " << status << "; document " << doc;
        }
    }


}  // namespace

    Status AuthzManagerExternalStateMongod::_initializeRoleGraph() {
        boost::lock_guard<boost::mutex> lkInitialzeRoleGraph(_roleGraphMutex);
        switch (_roleGraphState) {
        case roleGraphStateInitial:
        case roleGraphStateHasCycle:
            break;
        case roleGraphStateConsistent:
            return Status(ErrorCodes::AlreadyInitialized,
                          "Role graph already initialized and consistent.");
        default:
            return Status(ErrorCodes::InternalError,
                          mongoutils::str::stream() << "Invalid role graph state " <<
                          _roleGraphState);
        }

        RoleGraph newRoleGraph;
        Status status = query(
                AuthorizationManager::rolesCollectionNamespace,
                BSONObj(),
                boost::bind(addRoleFromDocumentOrWarn, &newRoleGraph, _1));
        if (!status.isOK())
            return status;

        status = newRoleGraph.recomputePrivilegeData();

        RoleGraphState newState;
        if (status == ErrorCodes::GraphContainsCycle) {
            error() << "Inconsistent role graph during authorization manager intialization.  Only "
                "direct privileges available. " << status.reason();
            newState = roleGraphStateHasCycle;
            status = Status::OK();
        }
        else if (status.isOK()) {
            newState = roleGraphStateConsistent;
        }
        else {
            newState = roleGraphStateInitial;
            newRoleGraph = RoleGraph();
        }

        if (status.isOK()) {
            _roleGraph.swap(newRoleGraph);
            _roleGraphState = newState;
        }
        return status;
    }

    void AuthzManagerExternalStateMongod::logOp(
            const char* op,
            const char* ns,
            const BSONObj& o,
            BSONObj* o2,
            bool* b,
            bool fromMigrateUnused,
            const BSONObj* fullObjUnused) {

        if (ns == AuthorizationManager::rolesCollectionNamespace.ns() ||
            ns == AuthorizationManager::adminCommandNamespace.ns()) {

            boost::lock_guard<boost::mutex> lk(_roleGraphMutex);
            Status status = _roleGraph.handleLogOp(op, NamespaceString(ns), o, o2);

            if (status == ErrorCodes::OplogOperationUnsupported) {
                _roleGraph = RoleGraph();
                _roleGraphState = roleGraphStateInitial;
                error() << "Unsupported modification to roles collection in oplog; "
                    "TODO how to remedy. " << status << " Oplog entry: " << op;
            }
            else if (!status.isOK()) {
                warning() << "Skipping bad update to roles collection in oplog. " << status <<
                    " Oplog entry: " << op;
            }
            status = _roleGraph.recomputePrivilegeData();
            if (status == ErrorCodes::GraphContainsCycle) {
                _roleGraphState = roleGraphStateHasCycle;
                error() << "Inconsistent role graph during authorization manager intialization.  "
                    "Only direct privileges available. " << status.reason() <<
                    " after applying oplog entry " << op;
            }
            else if (!status.isOK()) {
                _roleGraphState = roleGraphStateInitial;
                error() << "Error updating role graph; only builtin roles available. "
                    "TODO how to remedy. " << status << " Oplog entry: " << op;
            }
            else {
                _roleGraphState = roleGraphStateConsistent;
            }
        }
    }

} // namespace mongo
