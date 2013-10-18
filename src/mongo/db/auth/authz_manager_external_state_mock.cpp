/*
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

#include "mongo/db/auth/authz_manager_external_state_mock.h"

#include <string>

#include "mongo/base/status.h"
#include "mongo/bson/mutable/algorithm.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/bson/mutable/element.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/ops/update_driver.h"
#include "mongo/platform/unordered_set.h"
#include "mongo/util/map_util.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace {
    void addRoleNameToObjectElement(mutablebson::Element object, const RoleName& role) {
        fassert(17175, object.appendString(AuthorizationManager::ROLE_NAME_FIELD_NAME, role.getRole()));
        fassert(17176, object.appendString(AuthorizationManager::ROLE_SOURCE_FIELD_NAME, role.getDB()));
    }

    void addRoleNameObjectsToArrayElement(mutablebson::Element array, RoleNameIterator roles) {
        for (; roles.more(); roles.next()) {
            mutablebson::Element roleElement = array.getDocument().makeElementObject("");
            addRoleNameToObjectElement(roleElement, roles.get());
            fassert(17177, array.pushBack(roleElement));
        }
    }

    void addPrivilegeObjectsOrWarningsToArrayElement(mutablebson::Element privilegesElement,
                                                     mutablebson::Element warningsElement,
                                                     const PrivilegeVector& privileges) {
        std::string errmsg;
        for (size_t i = 0; i < privileges.size(); ++i) {
            ParsedPrivilege pp;
            if (ParsedPrivilege::privilegeToParsedPrivilege(privileges[i], &pp, &errmsg)) {
                fassert(17178, privilegesElement.appendObject("", pp.toBSON()));
            } else {
                fassert(17179,
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


    Status AuthzManagerExternalStateMock::initialize() {
        return Status::OK();
    }

    Status AuthzManagerExternalStateMock::getStoredAuthorizationVersion(int* outVersion) {
        if (_authzVersion < 0) {
            return Status(ErrorCodes::UnknownError,
                          "Mock configured to fail getStoredAuthorizationVersion()");
        }
        *outVersion = _authzVersion;
        return Status::OK();
    }

    Status AuthzManagerExternalStateMock::getUserDescription(
            const UserName& userName, BSONObj* result) {
        BSONObj privDoc;
        Status status = _findUser(
                "admin.system.users",
                BSON(AuthorizationManager::USER_NAME_FIELD_NAME << userName.getUser() <<
                     AuthorizationManager::USER_SOURCE_FIELD_NAME << userName.getDB()),
                &privDoc);
        if (!status.isOK())
            return status;

        unordered_set<RoleName> indirectRoles;
        PrivilegeVector allPrivileges;
        for (BSONObjIterator iter(privDoc["roles"].Obj()); iter.more(); iter.next()) {
            if (!(*iter)["hasRole"].trueValue())
                continue;
            RoleName roleName((*iter)[AuthorizationManager::ROLE_NAME_FIELD_NAME].str(),
                              (*iter)[AuthorizationManager::ROLE_SOURCE_FIELD_NAME].str());
            indirectRoles.insert(roleName);
            for (RoleNameIterator subordinates = _roleGraph.getIndirectSubordinates(
                         roleName);
                 subordinates.more();
                 subordinates.next()) {

                indirectRoles.insert(subordinates.get());
            }
            const PrivilegeVector& rolePrivileges(_roleGraph.getAllPrivileges(roleName));
            for (PrivilegeVector::const_iterator priv = rolePrivileges.begin(),
                     end = rolePrivileges.end();
                 priv != end;
                 ++priv) {

                Privilege::addPrivilegeToPrivilegeVector(&allPrivileges, *priv);
            }
        }

        mutablebson::Document userDoc(privDoc, mutablebson::Document::kInPlaceDisabled);
        mutablebson::Element indirectRolesElement = userDoc.makeElementArray("indirectRoles");
        mutablebson::Element privilegesElement = userDoc.makeElementArray("privileges");
        mutablebson::Element warningsElement = userDoc.makeElementArray("warnings");
        fassert(17180, userDoc.root().pushBack(privilegesElement));
        fassert(17181, userDoc.root().pushBack(indirectRolesElement));

        addRoleNameObjectsToArrayElement(indirectRolesElement,
                                         makeRoleNameIteratorForContainer(indirectRoles));
        addPrivilegeObjectsOrWarningsToArrayElement(
                privilegesElement, warningsElement, allPrivileges);
        if (warningsElement.hasChildren()) {
            fassert(17182, userDoc.root().pushBack(warningsElement));
        }
        *result = userDoc.getObject();
        return Status::OK();
    }

    Status AuthzManagerExternalStateMock::getRoleDescription(
            const RoleName& roleName, BSONObj* result) {
        return Status(ErrorCodes::RoleNotFound, "Not implemented");
    }


    Status AuthzManagerExternalStateMock::updatePrivilegeDocument(const UserName& user,
                                                                  const BSONObj& updateObj,
                                                                  const BSONObj&) {
        return Status(ErrorCodes::InternalError, "Not implemented in mock.");
    }

    Status AuthzManagerExternalStateMock::removePrivilegeDocuments(const BSONObj& query,
                                                                   const BSONObj&,
                                                                   int* numRemoved) {
        return Status(ErrorCodes::InternalError, "Not implemented in mock.");
    }

    Status AuthzManagerExternalStateMock::insertPrivilegeDocument(const std::string& dbname,
                                                                  const BSONObj& userObj,
                                                                  const BSONObj& writeConcern) {
        NamespaceString usersCollection("admin.system.users");
        return insert(usersCollection, userObj, writeConcern);
    }

    void AuthzManagerExternalStateMock::clearPrivilegeDocuments() {
        _documents.clear();
    }

    Status AuthzManagerExternalStateMock::getAllDatabaseNames(
            std::vector<std::string>* dbnames) {
        unordered_set<std::string> dbnameSet;
        NamespaceDocumentMap::const_iterator it;
        for (it = _documents.begin(); it != _documents.end(); ++it) {
            dbnameSet.insert(it->first.db().toString());
        }
        *dbnames = std::vector<std::string>(dbnameSet.begin(), dbnameSet.end());
        return Status::OK();
    }

    Status AuthzManagerExternalStateMock::getAllV1PrivilegeDocsForDB(
            const std::string& dbname, BSONObjCollection* privDocs) {
        NamespaceDocumentMap::const_iterator iter =
            _documents.find(NamespaceString(dbname + ".system.users"));
        if (iter == _documents.end())
            return Status::OK();  // No system.users collection in DB "dbname".
        const BSONObjCollection& dbDocs = iter->second;
        for (BSONObjCollection::const_iterator it = dbDocs.begin(); it != dbDocs.end(); ++it) {
            privDocs->push_back(*it);
        }
        return Status::OK();
    }

    Status AuthzManagerExternalStateMock::_findUser(
            const std::string& usersNamespace,
            const BSONObj& query,
            BSONObj* result) {
        if (!findOne(NamespaceString(usersNamespace), query, result).isOK()) {
            return Status(ErrorCodes::UserNotFound,
                          "No matching user for query " + query.toString());
        }
        return Status::OK();
    }

    Status AuthzManagerExternalStateMock::findOne(
            const NamespaceString& collectionName,
            const BSONObj& query,
            BSONObj* result) {
        BSONObjCollection::iterator iter;
        Status status = _findOneIter(collectionName, query, &iter);
        if (!status.isOK())
            return status;
        *result = iter->copy();
        return Status::OK();
    }

    Status AuthzManagerExternalStateMock::query(
            const NamespaceString& collectionName,
            const BSONObj& query,
            const BSONObj&,
            const boost::function<void(const BSONObj&)>& resultProcessor) {
        std::vector<BSONObjCollection::iterator> iterVector;
        Status status = _queryVector(collectionName, query, &iterVector);
        if (!status.isOK()) {
            return status;
        }
        try {
            for (std::vector<BSONObjCollection::iterator>::iterator it = iterVector.begin();
                 it != iterVector.end(); ++it) {
                resultProcessor(**it);
            }
        }
        catch (const DBException& ex) {
            status = ex.toStatus();
        }
        return status;
    }

    Status AuthzManagerExternalStateMock::insert(
            const NamespaceString& collectionName,
            const BSONObj& document,
            const BSONObj&) {
        _documents[collectionName].push_back(document.copy());
        return Status::OK();
    }

    Status AuthzManagerExternalStateMock::updateOne(
            const NamespaceString& collectionName,
            const BSONObj& query,
            const BSONObj& updatePattern,
            bool upsert,
            const BSONObj& writeConcern) {

        namespace mmb = mutablebson;
        UpdateDriver::Options updateOptions;
        updateOptions.upsert = upsert;
        UpdateDriver driver(updateOptions);
        Status status = driver.parse(updatePattern);
        if (!status.isOK())
            return status;

        BSONObjCollection::iterator iter;
        status = _findOneIter(collectionName, query, &iter);
        mmb::Document document;
        if (status.isOK()) {
            document.reset(*iter, mmb::Document::kInPlaceDisabled);
            status = driver.update(StringData(), &document, NULL);
            if (!status.isOK())
                return status;
            *iter = document.getObject().copy();
            return Status::OK();
        }
        else if (status == ErrorCodes::NoMatchingDocument && upsert) {
            if (query.hasField("_id")) {
                document.root().appendElement(query["_id"]);
            }
            status = driver.createFromQuery(query, document);
            if (!status.isOK()) {
                return status;
            }
            status = driver.update(StringData(), &document, NULL);
            if (!status.isOK()) {
                return status;
            }
            return insert(collectionName, document.getObject(), writeConcern);
        }
        else {
            return status;
        }
    }

    Status AuthzManagerExternalStateMock::update(const NamespaceString& collectionName,
                                                 const BSONObj& query,
                                                 const BSONObj& updatePattern,
                                                 bool upsert,
                                                 bool multi,
                                                 const BSONObj& writeConcern,
                                                 int* numUpdated) {
        return Status(ErrorCodes::InternalError,
                      "AuthzManagerExternalStateMock::update not implemented in mock.");
    }

    Status AuthzManagerExternalStateMock::remove(
            const NamespaceString& collectionName,
            const BSONObj& query,
            const BSONObj&,
            int* numRemoved) {
        int n = 0;
        BSONObjCollection::iterator iter;
        while (_findOneIter(collectionName, query, &iter).isOK()) {
            _documents[collectionName].erase(iter);
            ++n;
        }
        *numRemoved = n;
        return Status::OK();
    }

    Status AuthzManagerExternalStateMock::createIndex(
            const NamespaceString& collectionName,
            const BSONObj& pattern,
            bool unique,
            const BSONObj&) {
        return Status::OK();
    }

    Status AuthzManagerExternalStateMock::dropCollection(const NamespaceString& collectionName,
                                                         const BSONObj&) {
        _documents.erase(collectionName);
        return Status::OK();
    }

    Status AuthzManagerExternalStateMock::renameCollection(const NamespaceString& oldName,
                                                           const NamespaceString& newName,
                                                           const BSONObj& writeConcern) {
        if (_documents.count(oldName) == 0) {
            return Status(ErrorCodes::NamespaceNotFound,
                          "No collection to rename named " + oldName.ns());
        }
        std::swap(_documents[newName], _documents[oldName]);
        return dropCollection(oldName, writeConcern);
    }

    Status AuthzManagerExternalStateMock::copyCollection(const NamespaceString& fromName,
                                                         const NamespaceString& toName,
                                                         const BSONObj&) {
        if (_documents.count(fromName) == 0) {
            return Status(ErrorCodes::NamespaceNotFound,
                          "No collection to copy named " + fromName.ns());
        }
        if (_documents.count(toName) > 0) {
            return Status(ErrorCodes::NamespaceExists,
                          "Cannot copy into existing namespace " + fromName.ns());
        }

        _documents[toName] = _documents[fromName];
        return Status::OK();
    }

    bool AuthzManagerExternalStateMock::tryAcquireAuthzUpdateLock(const StringData&) {
        return true;
    }

    void AuthzManagerExternalStateMock::releaseAuthzUpdateLock() {}

    std::vector<BSONObj> AuthzManagerExternalStateMock::getCollectionContents(
            const NamespaceString& collectionName) {
        return mapFindWithDefault(_documents, collectionName, std::vector<BSONObj>());
    }

    Status AuthzManagerExternalStateMock::_findOneIter(
            const NamespaceString& collectionName,
            const BSONObj& query,
            BSONObjCollection::iterator* result) {
        std::vector<BSONObjCollection::iterator> iterVector;
        Status status = _queryVector(collectionName, query, &iterVector);
        if (!status.isOK()) {
            return status;
        }
        if (!iterVector.size()) {
            return Status(ErrorCodes::NoMatchingDocument, "No matching document");
        }
        *result = iterVector.front();
        return Status::OK();
    }

    Status AuthzManagerExternalStateMock::_queryVector(
            const NamespaceString& collectionName,
            const BSONObj& query,
            std::vector<BSONObjCollection::iterator>* result) {

        StatusWithMatchExpression parseResult = MatchExpressionParser::parse(query);
        if (!parseResult.isOK()) {
            return parseResult.getStatus();
        }
        MatchExpression* matcher = parseResult.getValue();

        NamespaceDocumentMap::iterator mapIt = _documents.find(collectionName);
        if (mapIt == _documents.end())
            return Status(ErrorCodes::NoMatchingDocument,
                          "No collection named " + collectionName.ns());

        for (BSONObjCollection::iterator vecIt = mapIt->second.begin();
             vecIt != mapIt->second.end();
             ++vecIt) {

            if (matcher->matchesBSON(*vecIt)) {
                result->push_back(vecIt);
            }
        }
        return Status::OK();
    }

} // namespace mongo
