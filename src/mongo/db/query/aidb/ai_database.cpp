/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/query/aidb/ai_database.h"

#include "mongo/db/catalog_raii.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/util/invariant.h"

namespace mongo::ai {

namespace {
auto currentDatabaseNameDecoration = OperationContext::declareDecoration<std::string>();
}  // namespace

void Database::setCurrentDbName(OperationContext* opCtx, const std::string& name) {
    currentDatabaseNameDecoration(opCtx) = name;
}

std::string Database::getCurrentDbName(OperationContext* opCtx) {
    return currentDatabaseNameDecoration(opCtx);
}

void Database::createCollection(const NamespaceString& collectionNamespace) {
    auto opCtx = operationContext();

    CollectionOptions options;

    AutoGetCollection autoColl(opCtx, collectionNamespace, MODE_IX);
    auto* db = autoColl.ensureDbExists();
    WriteUnitOfWork wuow(opCtx);
    invariant(db->createCollection(opCtx, collectionNamespace, options));
    wuow.commit();
}

void Database::ensureCollection(const NamespaceString& collectionNamespace) {
    auto opCtx = operationContext();

    CollectionOptions options;

    AutoGetCollection autoColl(opCtx, collectionNamespace, MODE_IX);
    if (!autoColl) {
        auto* db = autoColl.ensureDbExists();
        WriteUnitOfWork wuow(opCtx);
        invariant(db->createCollection(opCtx, collectionNamespace, options));
        wuow.commit();
    }
}

void Database::createIndex(const NamespaceString& collectionNamespace,
                           BSONObj indexKey,
                           StringData indexName) {
    OperationContext* opCtx = operationContext();
    BSONObj indexSpec = makeIndexSpec(indexKey, indexName);

    AutoGetCollection autoColl{opCtx, collectionNamespace, MODE_X};
    uassert(7777720, "collection not found", autoColl);

    if (autoColl->isEmpty(opCtx)) {
        WriteUnitOfWork wuow{opCtx};
        Collection* collection = autoColl.getWritableCollection();
        IndexCatalog* indexCatalog = collection->getIndexCatalog();
        uassertStatusOK(indexCatalog->createIndexOnEmptyCollection(opCtx, collection, indexSpec));
        wuow.commit();
    } else {
        IndexBuildsCoordinator* indexBuildsCoordinator = IndexBuildsCoordinator::get(opCtx);
        indexBuildsCoordinator->createIndex(opCtx,
                                            autoColl->uuid(),
                                            indexSpec,
                                            IndexBuildsManager::IndexConstraints::kEnforce,
                                            /*fromMigrate*/ false);
    }
}

void Database::insertDocuments(const NamespaceString& collectionNamespace,
                               const std::vector<BSONObj>& docs) {
    OperationContext* opCtx = operationContext();

    std::vector<InsertStatement> statements{};
    statements.reserve(docs.size());
    for (const BSONObj& doc : docs) {
        statements.emplace_back(doc);
    }

    AutoGetCollection autoColl{opCtx, collectionNamespace, MODE_IX};
    const CollectionPtr& coll = autoColl.getCollection();

    WriteUnitOfWork wuow{opCtx};
    invariant(coll->insertDocuments(opCtx, statements.begin(), statements.end(), nullptr));
    wuow.commit();
}

BSONObj Database::makeIndexSpec(BSONObj indexKey, StringData indexName) {
    return BSON("v" << int(IndexDescriptor::kLatestIndexVersion) << "key" << indexKey << "name"
                    << indexName);
}

NamespaceString Database::getNamespaceString(const std::string& collectionName) const {
    return {getCurrentName(), collectionName};
}
}  // namespace mongo::ai
