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


#include "mongo/platform/basic.h"

#include "mongo/db/views/durable_view_catalog.h"

#include <string>

#include "mongo/db/audit.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/curop.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/views/view_catalog_helpers.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/string_map.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {

namespace {
void validateViewDefinitionBSON(OperationContext* opCtx,
                                const BSONObj& viewDefinition,
                                const DatabaseName& dbName) {
    // Internal callers should always pass in a valid 'dbName' against which to compare the
    // 'viewDefinition'.
    invariant(NamespaceString::validDBName(dbName));

    bool valid = true;

    for (const BSONElement& e : viewDefinition) {
        std::string name(e.fieldName());
        valid &= name == "_id" || name == "viewOn" || name == "pipeline" || name == "collation" ||
            name == "timeseries";
    }

    const auto viewName = viewDefinition["_id"].str();
    const auto viewNameIsValid = NamespaceString::validCollectionComponent(viewName) &&
        NamespaceString::validDBName(nsToDatabaseSubstring(viewName));
    valid &= viewNameIsValid;

    // Only perform validation via NamespaceString if the collection name has been determined to
    // be valid. If not valid then the NamespaceString constructor will uassert.
    if (viewNameIsValid) {
        NamespaceString viewNss(viewName);
        valid &= viewNss.isValid() && viewNss.dbName() == dbName;
    }

    valid &= NamespaceString::validCollectionName(viewDefinition["viewOn"].str());

    const bool hasPipeline = viewDefinition.hasField("pipeline");
    valid &= hasPipeline;
    if (hasPipeline) {
        valid &= viewDefinition["pipeline"].type() == mongo::Array;
    }

    valid &= (!viewDefinition.hasField("collation") ||
              viewDefinition["collation"].type() == BSONType::Object);

    valid &= !viewDefinition.hasField("timeseries") ||
        viewDefinition["timeseries"].type() == BSONType::Object;

    uassert(ErrorCodes::InvalidViewDefinition,
            str::stream() << "found invalid view definition " << viewDefinition["_id"]
                          << " while reading '"
                          << NamespaceString(dbName, NamespaceString::kSystemDotViewsCollectionName)
                          << "'",
            valid);
}
}  // namespace

// DurableViewCatalog

void DurableViewCatalog::onExternalChange(OperationContext* opCtx, const NamespaceString& name) {
    dassert(opCtx->lockState()->isDbLockedForMode(name.db(), MODE_IX));
    dassert(opCtx->lockState()->isCollectionLockedForMode(
        NamespaceString(name.db(), NamespaceString::kSystemDotViewsCollectionName), MODE_X));

    // On an external change, an invalid view definition can be detected when the view catalog
    // is reloaded. This will prevent any further usage of the views for this database until the
    // invalid view definitions are removed.
    auto catalog = CollectionCatalog::get(opCtx);
    catalog->reloadViews(opCtx, name.dbName()).ignore();
}

Status DurableViewCatalog::onExternalInsert(OperationContext* opCtx,
                                            const BSONObj& doc,
                                            const NamespaceString& name) {
    try {
        validateViewDefinitionBSON(opCtx, doc, name.dbName());
    } catch (const DBException& e) {
        return e.toStatus();
    }

    auto catalog = CollectionCatalog::get(opCtx);
    NamespaceString viewName(doc.getStringField("_id"));
    NamespaceString viewOn(name.db(), doc.getStringField("viewOn"));
    BSONArray pipeline(doc.getObjectField("pipeline"));
    BSONObj collation(doc.getObjectField("collation"));

    return catalog->createView(opCtx,
                               viewName,
                               viewOn,
                               pipeline,
                               collation,
                               view_catalog_helpers::validatePipeline,
                               CollectionCatalog::ViewUpsertMode::kAlreadyDurableView);
}

void DurableViewCatalog::onSystemViewsCollectionDrop(OperationContext* opCtx,
                                                     const NamespaceString& name) {
    dassert(opCtx->lockState()->isDbLockedForMode(name.db(), MODE_IX));
    dassert(opCtx->lockState()->isCollectionLockedForMode(
        NamespaceString(name.db(), NamespaceString::kSystemDotViewsCollectionName), MODE_X));
    dassert(name.coll() == NamespaceString::kSystemDotViewsCollectionName);

    auto catalog = CollectionCatalog::get(opCtx);
    const DatabaseName& dbName = name.dbName();

    // First, iterate through the views on this database and audit them before they are dropped.
    catalog->iterateViews(opCtx,
                          dbName,
                          [&](const ViewDefinition& view) -> bool {
                              audit::logDropView(opCtx->getClient(),
                                                 view.name(),
                                                 view.viewOn().ns(),
                                                 view.pipeline(),
                                                 ErrorCodes::OK);
                              return true;
                          },
                          ViewCatalogLookupBehavior::kAllowInvalidViews);

    // If the 'system.views' collection is dropped, we need to clear the in-memory state of the
    // view catalog.
    catalog->clearViews(opCtx, dbName);
}

// DurableViewCatalogImpl

const std::string& DurableViewCatalogImpl::getName() const {
    return _db->name().toString();
}

void DurableViewCatalogImpl::iterate(OperationContext* opCtx, Callback callback) {
    _iterate(opCtx, callback, ViewCatalogLookupBehavior::kValidateViews);
}

void DurableViewCatalogImpl::iterateIgnoreInvalidEntries(OperationContext* opCtx,
                                                         Callback callback) {
    _iterate(opCtx, callback, ViewCatalogLookupBehavior::kAllowInvalidViews);
}

void DurableViewCatalogImpl::_iterate(OperationContext* opCtx,
                                      Callback callback,
                                      ViewCatalogLookupBehavior lookupBehavior) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(_db->getSystemViewsName(), MODE_IS));

    CollectionPtr systemViews = CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(
        opCtx, _db->getSystemViewsName());
    if (!systemViews) {
        return;
    }

    auto cursor = systemViews->getCursor(opCtx);
    while (auto record = cursor->next()) {
        BSONObj viewDefinition;
        try {
            viewDefinition = _validateViewDefinition(opCtx, record->data);
            uassertStatusOK(callback(viewDefinition));
        } catch (const ExceptionFor<ErrorCodes::InvalidViewDefinition>&) {
            if (lookupBehavior == ViewCatalogLookupBehavior::kValidateViews) {
                throw;
            }
        }
    }
}

BSONObj DurableViewCatalogImpl::_validateViewDefinition(OperationContext* opCtx,
                                                        const RecordData& recordData) {
    // Check the document is valid BSON, with only the expected fields.
    // Use the latest BSON validation version. Existing view definitions are allowed to contain
    // decimal data even if decimal is disabled.
    fassert(40224, validateBSON(recordData.data(), recordData.size()));
    BSONObj viewDefinition = recordData.toBson();

    validateViewDefinitionBSON(opCtx, viewDefinition, _db->name());
    return viewDefinition;
}

void DurableViewCatalogImpl::upsert(OperationContext* opCtx,
                                    const NamespaceString& name,
                                    const BSONObj& view) {
    dassert(opCtx->lockState()->isDbLockedForMode(_db->name().db(), MODE_IX));
    dassert(opCtx->lockState()->isCollectionLockedForMode(name, MODE_IX));

    NamespaceString systemViewsNs(_db->getSystemViewsName());
    dassert(opCtx->lockState()->isCollectionLockedForMode(systemViewsNs, MODE_X));

    const CollectionPtr& systemViews =
        CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, systemViewsNs);
    invariant(systemViews);

    RecordId id = Helpers::findOne(opCtx, systemViews, BSON("_id" << name.ns()));

    Snapshotted<BSONObj> oldView;
    if (!id.isValid() || !systemViews->findDoc(opCtx, id, &oldView)) {
        LOGV2_DEBUG(22544,
                    2,
                    "Insert view to system views catalog",
                    "view"_attr = view,
                    "viewCatalog"_attr = _db->getSystemViewsName());
        uassertStatusOK(
            systemViews->insertDocument(opCtx, InsertStatement(view), &CurOp::get(opCtx)->debug()));
    } else {
        CollectionUpdateArgs args;
        args.update = view;
        args.criteria = BSON("_id" << name.ns());

        const bool assumeIndexesAreAffected = true;
        systemViews->updateDocument(
            opCtx, id, oldView, view, assumeIndexesAreAffected, &CurOp::get(opCtx)->debug(), &args);
    }
}

void DurableViewCatalogImpl::remove(OperationContext* opCtx, const NamespaceString& name) {
    dassert(opCtx->lockState()->isDbLockedForMode(_db->name().db(), MODE_IX));
    dassert(opCtx->lockState()->isCollectionLockedForMode(name, MODE_IX));

    CollectionPtr systemViews = CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(
        opCtx, _db->getSystemViewsName());
    dassert(opCtx->lockState()->isCollectionLockedForMode(systemViews->ns(), MODE_X));

    if (!systemViews)
        return;
    RecordId id = Helpers::findOne(opCtx, systemViews, BSON("_id" << name.ns()));
    if (!id.isValid())
        return;

    LOGV2_DEBUG(22545,
                2,
                "Remove view from system views catalog",
                "view"_attr = name,
                "viewCatalog"_attr = _db->getSystemViewsName());
    systemViews->deleteDocument(opCtx, kUninitializedStmtId, id, &CurOp::get(opCtx)->debug());
}
}  // namespace mongo
