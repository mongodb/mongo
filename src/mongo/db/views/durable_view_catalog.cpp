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
#include "mongo/db/dbhelpers.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

namespace mongo {

void DurableViewCatalogImpl::iterate(OperationContext* txn, Callback callback) {
    dassert(txn->lockState()->isDbLockedForMode(_db->name(), MODE_X));
    Collection* systemViews = _db->getCollection(_db->getSystemViewsName());
    if (!systemViews)
        return;

    auto cursor = systemViews->getCursor(txn);
    while (auto record = cursor->next()) {
        RecordData& data = record->data;

        // Check the document is valid BSON, with only the expected fields.
        fassertStatusOK(40224, validateBSON(data.data(), data.size()));
        BSONObj viewDef = data.toBson();

        // Make sure we fail when new fields get added to the definition, so we fail safe in case
        // of future format upgrades.
        for (const BSONElement& e : viewDef) {
            std::string name(e.fieldName());
            fassert(40225, name == "_id" || name == "viewOn" || name == "pipeline");
        }
        NamespaceString viewName(viewDef["_id"].str());
        fassert(40226, viewName.db() == _db->name());

        callback(viewDef);
    }
}

void DurableViewCatalogImpl::insert(OperationContext* txn, const BSONObj& view) {
    dassert(txn->lockState()->isDbLockedForMode(_db->name(), MODE_X));
    Collection* systemViews = _db->getOrCreateCollection(txn, _db->getSystemViewsName());

    OpDebug* const opDebug = nullptr;
    const bool enforceQuota = false;
    LOG(2) << "insert view " << view << " in " << _db->getSystemViewsName();
    uassertStatusOK(systemViews->insertDocument(txn, view, opDebug, enforceQuota));
}

void DurableViewCatalogImpl::remove(OperationContext* txn, const NamespaceString& name) {
    dassert(txn->lockState()->isDbLockedForMode(_db->name(), MODE_X));
    Collection* systemViews = _db->getCollection(_db->getSystemViewsName());
    if (!systemViews)
        return;
    const bool requireIndex = false;
    RecordId id = Helpers::findOne(txn, systemViews, BSON("_id" << name.ns()), requireIndex);
    if (!id.isNormal())
        return;

    LOG(2) << "remove view " << name << " from " << _db->getSystemViewsName();
    OpDebug* const opDebug = nullptr;
    systemViews->deleteDocument(txn, id, opDebug);
}
}  // namespace mongo
