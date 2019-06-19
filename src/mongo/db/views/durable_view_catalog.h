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

#pragma once

#include <functional>
#include <string>

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/db/namespace_string.h"

namespace mongo {

class BSONObj;
class Database;
class OperationContext;
class RecordData;

/**
 * ViewCatalogLookupBehavior specifies whether a lookup into the view catalog should attempt to
 * validate the durable entries that currently exist within the catalog. This validation should
 * rarely be skipped.
 */
enum class ViewCatalogLookupBehavior { kValidateDurableViews, kAllowInvalidDurableViews };

/**
 * Interface for system.views collection operations associated with view catalog management.
 * All methods must be called from within a WriteUnitOfWork, and with the DBLock held in at
 * least intent mode.
 */
class DurableViewCatalog {
public:
    static constexpr StringData viewsCollectionName() {
        return NamespaceString::kSystemDotViewsCollectionName;
    }

    /**
     * Thread-safe method to mark a catalog name was changed. This will cause the in-memory
     * view catalog to be marked invalid
     */
    static void onExternalChange(OperationContext* opCtx, const NamespaceString& name);

    using Callback = std::function<Status(const BSONObj& view)>;
    virtual void iterate(OperationContext* opCtx, Callback callback) = 0;
    virtual void iterateIgnoreInvalidEntries(OperationContext* opCtx, Callback callback) = 0;
    virtual void upsert(OperationContext* opCtx,
                        const NamespaceString& name,
                        const BSONObj& view) = 0;
    virtual void remove(OperationContext* opCtx, const NamespaceString& name) = 0;
    virtual const std::string& getName() const = 0;
    virtual ~DurableViewCatalog() = default;
};

/**
 * Actual implementation of DurableViewCatalog for use by the Database class.
 * Implements durability through database operations on the system.views collection.
 */
class DurableViewCatalogImpl final : public DurableViewCatalog {
public:
    explicit DurableViewCatalogImpl(Database* db) : _db(db) {}

    void iterate(OperationContext* opCtx, Callback callback);

    void iterateIgnoreInvalidEntries(OperationContext* opCtx, Callback callback);

    void upsert(OperationContext* opCtx, const NamespaceString& name, const BSONObj& view);
    void remove(OperationContext* opCtx, const NamespaceString& name);
    const std::string& getName() const;

private:
    void _iterate(OperationContext* opCtx,
                  Callback callback,
                  ViewCatalogLookupBehavior lookupBehavior);

    BSONObj _validateViewDefinition(OperationContext* opCtx, const RecordData& recordData);

    Database* const _db;
};
}  // namespace mongo
