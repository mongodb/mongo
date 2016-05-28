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
#include "mongo/db/auth/authz_session_external_state_mock.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/matcher/extensions_callback_disallow_extensions.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/ops/update_driver.h"
#include "mongo/platform/unordered_set.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/map_util.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace {
void addRoleNameToObjectElement(mutablebson::Element object, const RoleName& role) {
    fassert(17175, object.appendString(AuthorizationManager::ROLE_NAME_FIELD_NAME, role.getRole()));
    fassert(17176, object.appendString(AuthorizationManager::ROLE_DB_FIELD_NAME, role.getDB()));
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
                        std::string(mongoutils::str::stream()
                                    << "Skipped privileges on resource "
                                    << privileges[i].getResourcePattern().toString()
                                    << ". Reason: "
                                    << errmsg)));
        }
    }
}
}  // namespace

AuthzManagerExternalStateMock::AuthzManagerExternalStateMock() : _authzManager(NULL) {}
AuthzManagerExternalStateMock::~AuthzManagerExternalStateMock() {}

void AuthzManagerExternalStateMock::setAuthorizationManager(AuthorizationManager* authzManager) {
    _authzManager = authzManager;
}

void AuthzManagerExternalStateMock::setAuthzVersion(int version) {
    OperationContextNoop opCtx;
    uassertStatusOK(
        updateOne(&opCtx,
                  AuthorizationManager::versionCollectionNamespace,
                  AuthorizationManager::versionDocumentQuery,
                  BSON("$set" << BSON(AuthorizationManager::schemaVersionFieldName << version)),
                  true,
                  BSONObj()));
}

std::unique_ptr<AuthzSessionExternalState>
AuthzManagerExternalStateMock::makeAuthzSessionExternalState(AuthorizationManager* authzManager) {
    return stdx::make_unique<AuthzSessionExternalStateMock>(authzManager);
}

Status AuthzManagerExternalStateMock::findOne(OperationContext* txn,
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
    OperationContext* txn,
    const NamespaceString& collectionName,
    const BSONObj& query,
    const BSONObj&,
    const stdx::function<void(const BSONObj&)>& resultProcessor) {
    std::vector<BSONObjCollection::iterator> iterVector;
    Status status = _queryVector(collectionName, query, &iterVector);
    if (!status.isOK()) {
        return status;
    }
    try {
        for (std::vector<BSONObjCollection::iterator>::iterator it = iterVector.begin();
             it != iterVector.end();
             ++it) {
            resultProcessor(**it);
        }
    } catch (const DBException& ex) {
        status = ex.toStatus();
    }
    return status;
}

Status AuthzManagerExternalStateMock::insert(OperationContext* txn,
                                             const NamespaceString& collectionName,
                                             const BSONObj& document,
                                             const BSONObj&) {
    BSONObj toInsert;
    if (document["_id"].eoo()) {
        BSONObjBuilder docWithIdBuilder;
        docWithIdBuilder.append("_id", OID::gen());
        docWithIdBuilder.appendElements(document);
        toInsert = docWithIdBuilder.obj();
    } else {
        toInsert = document.copy();
    }
    _documents[collectionName].push_back(toInsert);

    if (_authzManager) {
        _authzManager->logOp(txn, "i", collectionName.ns().c_str(), toInsert, NULL);
    }

    return Status::OK();
}

Status AuthzManagerExternalStateMock::insertPrivilegeDocument(OperationContext* txn,
                                                              const BSONObj& userObj,
                                                              const BSONObj& writeConcern) {
    return insert(txn, AuthorizationManager::usersCollectionNamespace, userObj, writeConcern);
}

Status AuthzManagerExternalStateMock::updateOne(OperationContext* txn,
                                                const NamespaceString& collectionName,
                                                const BSONObj& query,
                                                const BSONObj& updatePattern,
                                                bool upsert,
                                                const BSONObj& writeConcern) {
    namespace mmb = mutablebson;
    UpdateDriver::Options updateOptions;
    UpdateDriver driver(updateOptions);
    Status status = driver.parse(updatePattern);
    if (!status.isOK())
        return status;

    BSONObjCollection::iterator iter;
    status = _findOneIter(collectionName, query, &iter);
    mmb::Document document;
    if (status.isOK()) {
        document.reset(*iter, mmb::Document::kInPlaceDisabled);
        BSONObj logObj;
        status = driver.update(StringData(), &document, &logObj);
        if (!status.isOK())
            return status;
        BSONObj newObj = document.getObject().copy();
        *iter = newObj;
        BSONObj idQuery = driver.makeOplogEntryQuery(newObj, false);

        if (_authzManager) {
            _authzManager->logOp(txn, "u", collectionName.ns().c_str(), logObj, &idQuery);
        }

        return Status::OK();
    } else if (status == ErrorCodes::NoMatchingDocument && upsert) {
        if (query.hasField("_id")) {
            document.root().appendElement(query["_id"]);
        }
        status = driver.populateDocumentWithQueryFields(txn, query, NULL, document);
        if (!status.isOK()) {
            return status;
        }
        status = driver.update(StringData(), &document);
        if (!status.isOK()) {
            return status;
        }
        return insert(txn, collectionName, document.getObject(), writeConcern);
    } else {
        return status;
    }
}

Status AuthzManagerExternalStateMock::update(OperationContext* txn,
                                             const NamespaceString& collectionName,
                                             const BSONObj& query,
                                             const BSONObj& updatePattern,
                                             bool upsert,
                                             bool multi,
                                             const BSONObj& writeConcern,
                                             int* nMatched) {
    return Status(ErrorCodes::InternalError,
                  "AuthzManagerExternalStateMock::update not implemented in mock.");
}

Status AuthzManagerExternalStateMock::remove(OperationContext* txn,
                                             const NamespaceString& collectionName,
                                             const BSONObj& query,
                                             const BSONObj&,
                                             int* numRemoved) {
    int n = 0;
    BSONObjCollection::iterator iter;
    while (_findOneIter(collectionName, query, &iter).isOK()) {
        BSONObj idQuery = (*iter)["_id"].wrap();
        _documents[collectionName].erase(iter);
        ++n;

        if (_authzManager) {
            _authzManager->logOp(txn, "d", collectionName.ns().c_str(), idQuery, NULL);
        }
    }
    *numRemoved = n;
    return Status::OK();
}

std::vector<BSONObj> AuthzManagerExternalStateMock::getCollectionContents(
    const NamespaceString& collectionName) {
    return mapFindWithDefault(_documents, collectionName, std::vector<BSONObj>());
}

Status AuthzManagerExternalStateMock::_findOneIter(const NamespaceString& collectionName,
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
    CollatorInterface* collator = nullptr;
    StatusWithMatchExpression parseResult =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    if (!parseResult.isOK()) {
        return parseResult.getStatus();
    }
    const std::unique_ptr<MatchExpression> matcher = std::move(parseResult.getValue());

    NamespaceDocumentMap::iterator mapIt = _documents.find(collectionName);
    if (mapIt == _documents.end())
        return Status::OK();

    for (BSONObjCollection::iterator vecIt = mapIt->second.begin(); vecIt != mapIt->second.end();
         ++vecIt) {
        if (matcher->matchesBSON(*vecIt)) {
            result->push_back(vecIt);
        }
    }
    return Status::OK();
}

}  // namespace mongo
