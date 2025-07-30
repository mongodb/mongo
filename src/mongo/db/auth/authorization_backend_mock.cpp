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

#include "mongo/db/auth/authorization_backend_mock.h"

#include "mongo/db/exec/matcher/matcher.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/field_ref_set.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_with_placeholder.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/update/update_driver.h"

#include <string>
#include <utility>


namespace mongo::auth {

Status AuthorizationBackendMock::insertUserDocument(OperationContext* opCtx,
                                                    const BSONObj& userObj,
                                                    const BSONObj& writeConcern) {
    return insert(opCtx, NamespaceString::kAdminUsersNamespace, userObj, writeConcern);
}

Status AuthorizationBackendMock::insertRoleDocument(OperationContext* opCtx,
                                                    const BSONObj& roleObj,
                                                    const BSONObj& writeConcern) {
    return insert(opCtx, NamespaceString::kAdminRolesNamespace, roleObj, writeConcern);
}


Status AuthorizationBackendMock::findOne(OperationContext* opCtx,
                                         const NamespaceString& collectionName,
                                         const BSONObj& query,
                                         BSONObj* result) {
    if (_findsShouldFail) {
        return Status(ErrorCodes::UnknownError,
                      "findOne on admin.system.users set to fail in mock.");
    }

    BSONObjCollection::iterator iter;
    Status status = _findOneIter(opCtx, collectionName, query, &iter);
    if (!status.isOK())
        return status;
    *result = iter->copy();
    return Status::OK();
}

Status AuthorizationBackendMock::insert(OperationContext* opCtx,
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

    return Status::OK();
}

Status AuthorizationBackendMock::remove(OperationContext* opCtx,
                                        const NamespaceString& collectionName,
                                        const BSONObj& query,
                                        const BSONObj&,
                                        int* numRemoved) {
    int n = 0;
    BSONObjCollection::iterator iter;
    while (_findOneIter(opCtx, collectionName, query, &iter).isOK()) {
        BSONObj idQuery = (*iter)["_id"].wrap();
        _documents[collectionName].erase(iter);
        ++n;
    }
    *numRemoved = n;
    return Status::OK();
}

Status AuthorizationBackendMock::updateOne(OperationContext* opCtx,
                                           const NamespaceString& collectionName,
                                           const BSONObj& query,
                                           const BSONObj& updatePattern,
                                           bool upsert,
                                           const BSONObj& writeConcern) {
    namespace mmb = mutablebson;
    auto expCtx = ExpressionContextBuilder{}.opCtx(opCtx).ns(collectionName).build();
    UpdateDriver driver(std::move(expCtx));
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    driver.parse(write_ops::UpdateModification::parseFromClassicUpdate(updatePattern),
                 arrayFilters);

    BSONObjCollection::iterator iter;
    Status status = _findOneIter(opCtx, collectionName, query, &iter);
    mmb::Document document;
    if (status.isOK()) {
        document.reset(*iter, mmb::Document::kInPlaceDisabled);
        const bool validateForStorage = false;
        const FieldRefSet emptyImmutablePaths;
        const bool isInsert = false;
        BSONObj logObj;
        status = driver.update(opCtx,
                               StringData(),
                               &document,
                               validateForStorage,
                               emptyImmutablePaths,
                               isInsert,
                               &logObj);
        if (!status.isOK())
            return status;
        BSONObj newObj = document.getObject().copy();
        *iter = newObj;
        BSONElement idQuery = newObj["_id"_sd];
        BSONObj idQueryObj = idQuery.isABSONObj() ? idQuery.Obj() : BSON("_id" << idQuery);

        return Status::OK();
    } else if (status == ErrorCodes::NoMatchingDocument && upsert) {
        if (query.hasField("_id")) {
            document.root().appendElement(query["_id"]).transitional_ignore();
        }
        const FieldRef idFieldRef("_id");
        FieldRefSet immutablePaths;
        invariant(immutablePaths.insert(&idFieldRef));
        status = driver.populateDocumentWithQueryFields(opCtx, query, immutablePaths, document);
        if (!status.isOK()) {
            return status;
        }

        const bool validateForStorage = false;
        const FieldRefSet emptyImmutablePaths;
        const bool isInsert = false;
        status = driver.update(
            opCtx, StringData(), &document, validateForStorage, emptyImmutablePaths, isInsert);
        if (!status.isOK()) {
            return status;
        }
        return insert(opCtx, collectionName, document.getObject(), writeConcern);
    } else {
        return status;
    }
}


Status AuthorizationBackendMock::_findOneIter(OperationContext* opCtx,
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

Status AuthorizationBackendMock::_queryVector(OperationContext* opCtx,
                                              const NamespaceString& collectionName,
                                              const BSONObj& query,
                                              std::vector<BSONObjCollection::iterator>* result) {
    auto expCtx = ExpressionContextBuilder{}.opCtx(opCtx).ns(collectionName).build();
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
        if (exec::matcher::matchesBSON(matcher.get(), *vecIt)) {
            result->push_back(vecIt);
        }
    }
    return Status::OK();
}

}  // namespace mongo::auth
