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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/views/durable_view_catalog.h"

#include <string>

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/curop.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/views/view_catalog.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/string_map.h"

namespace mongo {

// DurableViewCatalog

void DurableViewCatalog::onExternalChange(OperationContext* opCtx, const NamespaceString& name) {
    dassert(opCtx->lockState()->isDbLockedForMode(name.db(), MODE_IX));
    dassert(opCtx->lockState()->isCollectionLockedForMode(
        NamespaceString(name.db(), NamespaceString::kSystemDotViewsCollectionName), MODE_X));
    auto databaseHolder = DatabaseHolder::get(opCtx);
    auto db = databaseHolder->getDb(opCtx, name.db());
    if (db) {
        // On an external change, an invalid view definition can be detected when the view catalog
        // is reloaded. This will prevent any further usage of the view catalog until the invalid
        // view definitions are removed. We use kValidateDurableViews here to catch any invalid view
        // definitions in the view catalog to make it unusable for subsequent callers.
        ViewCatalog* viewCatalog = ViewCatalog::get(db);
        if (viewCatalog->shouldIgnoreExternalChange(opCtx, name)) {
            return;
        }

        viewCatalog->reload(opCtx, ViewCatalogLookupBehavior::kValidateDurableViews).ignore();
    }
}

void DurableViewCatalog::onSystemViewsCollectionDrop(OperationContext* opCtx,
                                                     const NamespaceString& name) {
    dassert(opCtx->lockState()->isDbLockedForMode(name.db(), MODE_IX));
    dassert(opCtx->lockState()->isCollectionLockedForMode(
        NamespaceString(name.db(), NamespaceString::kSystemDotViewsCollectionName), MODE_X));
    dassert(name.coll() == NamespaceString::kSystemDotViewsCollectionName);

    auto databaseHolder = DatabaseHolder::get(opCtx);
    auto db = databaseHolder->getDb(opCtx, name.db());
    if (db) {
        // If the 'system.views' collection is dropped, we need to clear the in-memory state of the
        // view catalog.
        ViewCatalog* viewCatalog = ViewCatalog::get(db);
        viewCatalog->clear();
    }
}

// DurableViewCatalogImpl

const std::string& DurableViewCatalogImpl::getName() const {
    return _db->name();
}

void DurableViewCatalogImpl::iterate(OperationContext* opCtx, Callback callback) {
    _iterate(opCtx, callback, ViewCatalogLookupBehavior::kValidateDurableViews);
}

void DurableViewCatalogImpl::iterateIgnoreInvalidEntries(OperationContext* opCtx,
                                                         Callback callback) {
    _iterate(opCtx, callback, ViewCatalogLookupBehavior::kAllowInvalidDurableViews);
}

void DurableViewCatalogImpl::_iterate(OperationContext* opCtx,
                                      Callback callback,
                                      ViewCatalogLookupBehavior lookupBehavior) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(_db->getSystemViewsName(), MODE_IS));

    Collection* systemViews = _db->getCollection(opCtx, _db->getSystemViewsName());
    if (!systemViews) {
        return;
    }

    auto cursor = systemViews->getCursor(opCtx);
    while (auto record = cursor->next()) {
        BSONObj viewDefinition;
        try {
            viewDefinition = _validateViewDefinition(opCtx, record->data);
            uassertStatusOK(callback(viewDefinition));
        } catch (const ExceptionFor<ErrorCodes::InvalidViewDefinition>& ex) {
            if (lookupBehavior == ViewCatalogLookupBehavior::kValidateDurableViews) {
                throw ex;
            }
        }
    }
}

BSONObj DurableViewCatalogImpl::_validateViewDefinition(OperationContext* opCtx,
                                                        const RecordData& recordData) {
    // Check the document is valid BSON, with only the expected fields.
    // Use the latest BSON validation version. Existing view definitions are allowed to contain
    // decimal data even if decimal is disabled.
    fassert(40224, validateBSON(recordData.data(), recordData.size(), BSONVersion::kLatest));
    BSONObj viewDefinition = recordData.toBson();

    bool valid = true;

    for (const BSONElement& e : viewDefinition) {
        std::string name(e.fieldName());
        valid &= name == "_id" || name == "viewOn" || name == "pipeline" || name == "collation";
    }

    const auto viewName = viewDefinition["_id"].str();
    const auto viewNameIsValid = NamespaceString::validCollectionComponent(viewName) &&
        NamespaceString::validDBName(nsToDatabaseSubstring(viewName));
    valid &= viewNameIsValid;

    // Only perform validation via NamespaceString if the collection name has been determined to
    // be valid. If not valid then the NamespaceString constructor will uassert.
    if (viewNameIsValid) {
        NamespaceString viewNss(viewName);
        valid &= viewNss.isValid() && viewNss.db() == _db->name();
    }

    valid &= NamespaceString::validCollectionName(viewDefinition["viewOn"].str());

    const bool hasPipeline = viewDefinition.hasField("pipeline");
    valid &= hasPipeline;
    if (hasPipeline) {
        valid &= viewDefinition["pipeline"].type() == mongo::Array;
    }

    valid &= (!viewDefinition.hasField("collation") ||
              viewDefinition["collation"].type() == BSONType::Object);

    uassert(ErrorCodes::InvalidViewDefinition,
            str::stream() << "found invalid view definition " << viewDefinition["_id"]
                          << " while reading '"
                          << _db->getSystemViewsName()
                          << "'",
            valid);

    return viewDefinition;
}

void DurableViewCatalogImpl::upsert(OperationContext* opCtx,
                                    const NamespaceString& name,
                                    const BSONObj& view) {
    dassert(opCtx->lockState()->isDbLockedForMode(_db->name(), MODE_IX));
    dassert(opCtx->lockState()->isCollectionLockedForMode(name, MODE_IX));

    NamespaceString systemViewsNs(_db->getSystemViewsName());
    dassert(opCtx->lockState()->isCollectionLockedForMode(systemViewsNs, MODE_X));

    Collection* systemViews = _db->getCollection(opCtx, systemViewsNs);
    invariant(systemViews);

    const bool requireIndex = false;
    RecordId id = Helpers::findOne(opCtx, systemViews, BSON("_id" << name.ns()), requireIndex);

    Snapshotted<BSONObj> oldView;
    if (!id.isValid() || !systemViews->findDoc(opCtx, id, &oldView)) {
        LOG(2) << "insert view " << view << " into " << _db->getSystemViewsName();
        uassertStatusOK(
            systemViews->insertDocument(opCtx, InsertStatement(view), &CurOp::get(opCtx)->debug()));
    } else {
        CollectionUpdateArgs args;
        args.update = view;
        args.criteria = BSON("_id" << name.ns());
        args.fromMigrate = false;

        const bool assumeIndexesAreAffected = true;
        systemViews->updateDocument(
            opCtx, id, oldView, view, assumeIndexesAreAffected, &CurOp::get(opCtx)->debug(), &args);
    }
}

void DurableViewCatalogImpl::remove(OperationContext* opCtx, const NamespaceString& name) {
    dassert(opCtx->lockState()->isDbLockedForMode(_db->name(), MODE_IX));
    dassert(opCtx->lockState()->isCollectionLockedForMode(name, MODE_IX));

    Collection* systemViews = _db->getCollection(opCtx, _db->getSystemViewsName());
    dassert(opCtx->lockState()->isCollectionLockedForMode(systemViews->ns(), MODE_X));

    if (!systemViews)
        return;
    const bool requireIndex = false;
    RecordId id = Helpers::findOne(opCtx, systemViews, BSON("_id" << name.ns()), requireIndex);
    if (!id.isValid())
        return;

    LOG(2) << "remove view " << name << " from " << _db->getSystemViewsName();
    systemViews->deleteDocument(opCtx, kUninitializedStmtId, id, &CurOp::get(opCtx)->debug());
}
}  // namespace mongo
