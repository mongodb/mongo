/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
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

#include "mongo/db/auth/authz_manager_external_state_mock.h"

#include <string>
#include <utility>

#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>

#include "mongo/base/error_codes.h"
#include "mongo/base/shim.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/bson/mutable/element.h"
#include "mongo/bson/oid.h"
#include "mongo/db/auth/authorization_backend_interface.h"
#include "mongo/db/auth/authorization_backend_mock.h"
#include "mongo/db/auth/authz_session_external_state.h"
#include "mongo/db/auth/authz_session_external_state_mock.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/auth/role_name.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/field_ref_set.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/matcher/expression_with_placeholder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"
#include "mongo/db/update/update_driver.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/safe_num.h"

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
    using namespace fmt::literals;
    for (const auto& privilege : privileges) {
        try {
            fassert(17178, privilegesElement.appendObject("", privilege.toBSON()));
        } catch (const DBException& ex) {
            fassert(17179,
                    warningsElement.appendString(
                        "",
                        "Skipped privileges on resource {}. Reason: {}"_format(
                            privilege.getResourcePattern().toString(), ex.what())));
        }
    }
}

}  // namespace

AuthzManagerExternalStateMock::AuthzManagerExternalStateMock() {}
AuthzManagerExternalStateMock::~AuthzManagerExternalStateMock() {}

void AuthzManagerExternalStateMock::setAuthzVersion(OperationContext* opCtx, int version) {
    auto backendMock = reinterpret_cast<auth::AuthorizationBackendMock*>(
        auth::AuthorizationBackendInterface::get(opCtx->getService()));
    invariant(backendMock);
    return backendMock->setAuthzVersion(opCtx, version);
}

std::unique_ptr<AuthzSessionExternalState>
AuthzManagerExternalStateMock::makeAuthzSessionExternalState(Client* client) {
    auto ret = std::make_unique<AuthzSessionExternalStateMock>(client);
    if (!AuthorizationManager::get(client->getService())->isAuthEnabled()) {
        // Construct a `AuthzSessionExternalStateMock` structure that represents the default no-auth
        // state of a running mongod.
        ret->setReturnValueForShouldIgnoreAuthChecks(true);
    }
    return ret;
}

Status AuthzManagerExternalStateMock::findOne(OperationContext* opCtx,
                                              const NamespaceString& collectionName,
                                              const BSONObj& query,
                                              BSONObj* result) {
    auto backendMock = reinterpret_cast<auth::AuthorizationBackendMock*>(
        auth::AuthorizationBackendInterface::get(opCtx->getService()));
    invariant(backendMock);
    return backendMock->findOne(opCtx, collectionName, query, result);
}


bool AuthzManagerExternalStateMock::hasOne(OperationContext* opCtx,
                                           const NamespaceString& collectionName,
                                           const BSONObj& query) {
    BSONObjCollection::iterator iter;
    return _findOneIter(opCtx, collectionName, query, &iter).isOK();
}

Status AuthzManagerExternalStateMock::query(
    OperationContext* opCtx,
    const NamespaceString& collectionName,
    const BSONObj& query,
    const BSONObj&,
    const std::function<void(const BSONObj&)>& resultProcessor) {
    std::vector<BSONObjCollection::iterator> iterVector;
    Status status = _queryVector(opCtx, collectionName, query, &iterVector);
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

Status AuthzManagerExternalStateMock::insert(OperationContext* opCtx,
                                             const NamespaceString& collectionName,
                                             const BSONObj& document,
                                             const BSONObj&) {
    auto backendMock = reinterpret_cast<auth::AuthorizationBackendMock*>(
        auth::AuthorizationBackendInterface::get(opCtx->getService()));
    invariant(backendMock);
    return backendMock->insert(opCtx, collectionName, document, {});
}

Status AuthzManagerExternalStateMock::insertUserDocument(OperationContext* opCtx,
                                                         const BSONObj& userObj,
                                                         const BSONObj& writeConcern) {
    auto backendMock = reinterpret_cast<auth::AuthorizationBackendMock*>(
        auth::AuthorizationBackendInterface::get(opCtx->getService()));
    invariant(backendMock);
    return backendMock->insertUserDocument(opCtx, userObj, writeConcern);
}

Status AuthzManagerExternalStateMock::insertRoleDocument(OperationContext* opCtx,
                                                         const BSONObj& roleObj,
                                                         const BSONObj& writeConcern) {
    auto backendMock = reinterpret_cast<auth::AuthorizationBackendMock*>(
        auth::AuthorizationBackendInterface::get(opCtx->getService()));
    invariant(backendMock);
    return backendMock->insertRoleDocument(opCtx, roleObj, writeConcern);
}

Status AuthzManagerExternalStateMock::updateOne(OperationContext* opCtx,
                                                const NamespaceString& collectionName,
                                                const BSONObj& query,
                                                const BSONObj& updatePattern,
                                                bool upsert,
                                                const BSONObj& writeConcern) {
    auto backendMock = reinterpret_cast<auth::AuthorizationBackendMock*>(
        auth::AuthorizationBackendInterface::get(opCtx->getService()));
    invariant(backendMock);
    return backendMock->updateOne(
        opCtx, collectionName, query, updatePattern, upsert, writeConcern);
}

Status AuthzManagerExternalStateMock::update(OperationContext* opCtx,
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

Status AuthzManagerExternalStateMock::remove(OperationContext* opCtx,
                                             const NamespaceString& collectionName,
                                             const BSONObj& query,
                                             const BSONObj&,
                                             int* numRemoved) {
    auto backendMock = reinterpret_cast<auth::AuthorizationBackendMock*>(
        auth::AuthorizationBackendInterface::get(opCtx->getService()));
    invariant(backendMock);
    return backendMock->remove(opCtx, collectionName, query, {}, numRemoved);
}

std::vector<BSONObj> AuthzManagerExternalStateMock::getCollectionContents(
    const NamespaceString& collectionName) {
    auto iter = _documents.find(collectionName);
    if (iter != _documents.end())
        return iter->second;
    return {};
}

Status AuthzManagerExternalStateMock::_findOneIter(OperationContext* opCtx,
                                                   const NamespaceString& collectionName,
                                                   const BSONObj& query,
                                                   BSONObjCollection::iterator* result) {
    std::vector<BSONObjCollection::iterator> iterVector;
    Status status = _queryVector(opCtx, collectionName, query, &iterVector);
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
    OperationContext* opCtx,
    const NamespaceString& collectionName,
    const BSONObj& query,
    std::vector<BSONObjCollection::iterator>* result) {
    boost::intrusive_ptr<ExpressionContext> expCtx(
        new ExpressionContext(opCtx, std::unique_ptr<CollatorInterface>(nullptr), collectionName));
    StatusWithMatchExpression parseResult = MatchExpressionParser::parse(query, std::move(expCtx));
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
