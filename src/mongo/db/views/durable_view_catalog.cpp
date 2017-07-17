/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/views/durable_view_catalog.h"

#include <string>

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/string_map.h"

namespace mongo {

// DurableViewCatalog

void DurableViewCatalog::onExternalChange(OperationContext* opCtx, const NamespaceString& name) {
    dassert(opCtx->lockState()->isDbLockedForMode(name.db(), MODE_IX));
    Database* db = dbHolder().get(opCtx, name.db());

    if (db) {
        opCtx->recoveryUnit()->onCommit([db]() { db->getViewCatalog()->invalidate(); });
    }
}

// DurableViewCatalogImpl

const std::string& DurableViewCatalogImpl::getName() const {
    return _db->name();
}

Status DurableViewCatalogImpl::iterate(OperationContext* opCtx, Callback callback) {
    dassert(opCtx->lockState()->isDbLockedForMode(_db->name(), MODE_IS) ||
            opCtx->lockState()->isDbLockedForMode(_db->name(), MODE_IX));
    Collection* systemViews = _db->getCollection(opCtx, _db->getSystemViewsName());
    if (!systemViews)
        return Status::OK();

    Lock::CollectionLock lk(opCtx->lockState(), _db->getSystemViewsName(), MODE_IS);
    auto cursor = systemViews->getCursor(opCtx);
    while (auto record = cursor->next()) {
        RecordData& data = record->data;

        // Check the document is valid BSON, with only the expected fields.
        // Use the latest BSON validation version. Existing view definitions are allowed to contain
        // decimal data even if decimal is disabled.
        fassertStatusOK(40224, validateBSON(data.data(), data.size(), BSONVersion::kLatest));
        BSONObj viewDef = data.toBson();

        // Check read definitions for correct structure, and refuse reading past invalid
        // definitions. Ignore any further view definitions.
        bool valid = true;
        for (const BSONElement& e : viewDef) {
            std::string name(e.fieldName());
            valid &= name == "_id" || name == "viewOn" || name == "pipeline" || name == "collation";
        }

        const auto viewName = viewDef["_id"].str();
        const auto collectionNameIsValid = NamespaceString::validCollectionComponent(viewName);
        valid &= collectionNameIsValid;

        // Only perform validation via NamespaceString if the collection name has been determined to
        // be valid. If not valid then the NamespaceString constructor will uassert.
        if (collectionNameIsValid) {
            NamespaceString viewNss(viewName);
            valid &= viewNss.isValid() && viewNss.db() == _db->name();
        }

        valid &= NamespaceString::validCollectionName(viewDef["viewOn"].str());

        const bool hasPipeline = viewDef.hasField("pipeline");
        valid &= hasPipeline;
        if (hasPipeline) {
            valid &= viewDef["pipeline"].type() == mongo::Array;
        }

        valid &=
            (!viewDef.hasField("collation") || viewDef["collation"].type() == BSONType::Object);

        if (!valid) {
            return {ErrorCodes::InvalidViewDefinition,
                    str::stream() << "found invalid view definition " << viewDef["_id"]
                                  << " while reading '"
                                  << _db->getSystemViewsName()
                                  << "'"};
        }

        Status callbackStatus = callback(viewDef);
        if (!callbackStatus.isOK()) {
            return callbackStatus;
        }
    }
    return Status::OK();
}

void DurableViewCatalogImpl::upsert(OperationContext* opCtx,
                                    const NamespaceString& name,
                                    const BSONObj& view) {
    dassert(opCtx->lockState()->isDbLockedForMode(_db->name(), MODE_X));
    NamespaceString systemViewsNs(_db->getSystemViewsName());
    Collection* systemViews = _db->getOrCreateCollection(opCtx, systemViewsNs);

    const bool requireIndex = false;
    RecordId id = Helpers::findOne(opCtx, systemViews, BSON("_id" << name.ns()), requireIndex);

    const bool enforceQuota = true;
    Snapshotted<BSONObj> oldView;
    if (!id.isNormal() || !systemViews->findDoc(opCtx, id, &oldView)) {
        LOG(2) << "insert view " << view << " into " << _db->getSystemViewsName();
        uassertStatusOK(systemViews->insertDocument(
            opCtx, InsertStatement(view), &CurOp::get(opCtx)->debug(), enforceQuota));
    } else {
        OplogUpdateEntryArgs args;
        args.nss = systemViewsNs;
        args.update = view;
        args.criteria = BSON("_id" << name.ns());
        args.fromMigrate = false;

        const bool assumeIndexesAreAffected = true;
        auto res = systemViews->updateDocument(opCtx,
                                               id,
                                               oldView,
                                               view,
                                               enforceQuota,
                                               assumeIndexesAreAffected,
                                               &CurOp::get(opCtx)->debug(),
                                               &args);
        uassertStatusOK(res);
    }
}

void DurableViewCatalogImpl::remove(OperationContext* opCtx, const NamespaceString& name) {
    dassert(opCtx->lockState()->isDbLockedForMode(_db->name(), MODE_X));
    Collection* systemViews = _db->getCollection(opCtx, _db->getSystemViewsName());
    if (!systemViews)
        return;
    const bool requireIndex = false;
    RecordId id = Helpers::findOne(opCtx, systemViews, BSON("_id" << name.ns()), requireIndex);
    if (!id.isNormal())
        return;

    LOG(2) << "remove view " << name << " from " << _db->getSystemViewsName();
    systemViews->deleteDocument(opCtx, kUninitializedStmtId, id, &CurOp::get(opCtx)->debug());
}
}  // namespace mongo
