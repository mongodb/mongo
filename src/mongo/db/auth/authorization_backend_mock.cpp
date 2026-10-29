// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
#include <string_view>
#include <utility>


namespace mongo::auth {
using namespace std::literals::string_view_literals;

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
    std::map<std::string_view, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
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
        status = driver.update(opCtx,
                               std::string_view(),
                               &document,
                               validateForStorage,
                               emptyImmutablePaths,
                               isInsert);
        if (!status.isOK())
            return status;
        BSONObj newObj = document.getObject().copy();
        *iter = newObj;
        BSONElement idQuery = newObj["_id"sv];
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
        status = driver.update(opCtx,
                               std::string_view(),
                               &document,
                               validateForStorage,
                               emptyImmutablePaths,
                               isInsert);
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
