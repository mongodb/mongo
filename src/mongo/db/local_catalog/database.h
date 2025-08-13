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

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbcommands_gen.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/local_catalog/virtual_collection_options.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/optime.h"
#include "mongo/util/decorable.h"

namespace mongo {

/**
 * Represents a logical database containing Collections.
 *
 * The semantics for a const Database are that you can mutate individual collections but not add or
 * remove them.
 */
class Database : public Decorable<Database> {
public:
    /**
     * Creates the namespace 'ns' in the database 'db' according to 'options'. If
     * 'createDefaultIndexes' is true, creates the _id index for the collection (and the system
     * indexes, in the case of system collections). Creates the collection's _id index according
     * to 'idIndex', if it is non-empty.  When 'idIndex' is empty, creates the default _id index.
     * If present, 'catalogIdentifier' specifies how to persist and identify the new collection
     * through the catalog.
     */
    virtual Status userCreateNS(OperationContext* opCtx,
                                const NamespaceString& fullns,
                                CollectionOptions collectionOptions,
                                bool createDefaultIndexes = true,
                                const BSONObj& idIndex = BSONObj(),
                                bool fromMigrate = false,
                                const boost::optional<CreateCollCatalogIdentifier>&
                                    catalogIdentifier = boost::none) const = 0;

    /**
     * Creates the virtual namespace 'fullns' according to 'opts' and 'vopts'.
     */
    virtual Status userCreateVirtualNS(OperationContext* opCtx,
                                       const NamespaceString& fullns,
                                       CollectionOptions opts,
                                       const VirtualCollectionOptions& vopts) const = 0;

    Database() = default;

    // must call close first
    ~Database() override = default;

    inline Database(Database&&) = delete;
    inline Database& operator=(Database&&) = delete;

    /**
     * Sets up internal memory structures.
     */
    virtual void init(OperationContext* opCtx) = 0;

    virtual const DatabaseName& name() const = 0;

    virtual void getStats(OperationContext* opCtx,
                          DBStats* output,
                          bool includeFreeStorage,
                          double scale = 1) const = 0;

    /**
     * dropCollection() will refuse to drop system collections. Use dropCollectionEvenIfSystem() if
     * that is required.
     *
     * If we are applying a 'drop' oplog entry on a secondary, 'dropOpTime' will contain the optime
     * of the oplog entry.
     *
     * When fromMigrate is set, the related oplog entry will be marked with a 'fromMigrate' field to
     * reduce its visibility (e.g. in change streams).
     *
     * The caller should hold a DB X lock and ensure there are no index builds in progress on the
     * collection.
     * N.B. Namespace argument is passed by value as it may otherwise disappear or change.
     */
    virtual Status dropCollection(OperationContext* opCtx,
                                  NamespaceString nss,
                                  repl::OpTime dropOpTime = {},
                                  bool markFromMigrate = false) const = 0;

    virtual Status dropCollectionEvenIfSystem(OperationContext* opCtx,
                                              NamespaceString nss,
                                              repl::OpTime dropOpTime = {},
                                              bool markFromMigrate = false) const = 0;

    virtual Status dropView(OperationContext* opCtx, NamespaceString viewName) const = 0;

    /**
     * A MODE_IX collection lock must be held for this call. Throws a WriteConflictException error
     * if the collection already exists (say if another thread raced to create it).
     *
     * Surrounding writeConflictRetry loops must encompass checking that the collection exists as
     * well as creating it. Otherwise the loop will endlessly throw WCEs: the caller must check that
     * the collection exists to break free.
     */
    virtual Collection* createCollection(OperationContext* opCtx,
                                         const NamespaceString& nss,
                                         const CollectionOptions& options = CollectionOptions(),
                                         bool createDefaultIndexes = true,
                                         const BSONObj& idIndex = BSONObj(),
                                         bool fromMigrate = false) const = 0;

    /**
     * A MODE_IX collection lock must be held for this call. Throws a WriteConflictException error
     * if the collection already exists (say if another thread raced to create it).
     *
     * Surrounding writeConflictRetry loops must encompass checking that the collection exists as
     * well as creating it. Otherwise the loop will endlessly throw WCEs: the caller must check that
     * the collection exists to break free.
     */
    virtual Collection* createVirtualCollection(OperationContext* opCtx,
                                                const NamespaceString& nss,
                                                const CollectionOptions& opts,
                                                const VirtualCollectionOptions& vopts) const = 0;

    virtual StatusWith<std::unique_ptr<CollatorInterface>> validateCollator(
        OperationContext* opCtx, CollectionOptions& opts) const = 0;

    virtual Status createView(OperationContext* opCtx,
                              const NamespaceString& viewName,
                              const CollectionOptions& options) const = 0;

    /**
     * Arguments are passed by value as they otherwise would be changing as result of renaming.
     */
    virtual Status renameCollection(OperationContext* opCtx,
                                    NamespaceString fromNss,
                                    NamespaceString toNss,
                                    bool stayTemp) const = 0;

    virtual const NamespaceString& getSystemViewsName() const = 0;
};

}  // namespace mongo
