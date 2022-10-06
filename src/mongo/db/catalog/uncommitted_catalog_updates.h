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

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/views_for_database.h"
#include "mongo/db/database_name.h"
#include "mongo/db/views/view.h"
#include "mongo/util/uuid.h"

namespace mongo {

/**
 * Decoration on RecoveryUnit to store cloned Collections until they are committed or rolled back.
 */
class UncommittedCatalogUpdates {
public:
    struct Entry {
        enum class Action {
            // Created collection instance
            kCreatedCollection,
            // Writable clone
            kWritableCollection,
            // Marker to indicate that the namespace has been renamed
            kRenamedCollection,
            // Dropped collection instance
            kDroppedCollection,
            // Recreated collection after drop
            kRecreatedCollection,
            // Replaced views for a particular database
            kReplacedViewsForDatabase,
            // Add a view resource
            kAddViewResource,
            // Remove a view resource
            kRemoveViewResource,
            // Dropped index instance
            kDroppedIndex,
            // Opened a collection instance from an earlier point-in-time
            kOpenedCollection
        };

        boost::optional<UUID> uuid() const {
            if (action == Action::kCreatedCollection || action == Action::kWritableCollection ||
                action == Action::kRenamedCollection || action == Action::kOpenedCollection)
                return collection->uuid();
            return externalUUID;
        }

        // Type of action this entry has stored. Members below may or may not be set depending on
        // this member.
        Action action;

        // Storage for the actual collection.
        // Set for actions kWritableCollection, kCreatedCollection, kRecreatedCollection and nullptr
        // otherwise.
        std::shared_ptr<Collection> collection;

        // Store namespace separately to handle rename and drop without making writable first.
        // Set for all actions.
        NamespaceString nss;

        // External uuid when not accessible via collection.
        // Set for actions kDroppedCollection, kRecreatedCollection. boost::none otherwise.
        boost::optional<UUID> externalUUID;

        // New namespace this collection has been renamed to.
        // Set for action kRenamedCollection. Default constructed otherwise.
        NamespaceString renameTo;

        // New set of view information for a database.
        // Set for action kReplacedViewsForDatabase, boost::none otherwise.
        boost::optional<ViewsForDatabase> viewsForDb;

        // Storage for the actual index entry.
        // Set for action kDroppedIndex and nullptr otherwise.
        std::shared_ptr<IndexCatalogEntry> indexEntry;

        // Whether the collection or index entry is drop pending.
        // Set for actions kDroppedCollection and kDroppedIndex, boost::none otherwise.
        boost::optional<bool> isDropPending;
    };

    struct CollectionLookupResult {
        // True if the collection is currently being managed in this transaction.
        bool found;

        // Storage for the actual collection.
        // Set for actions kWritableCollection, kCreatedCollection, kRecreatedCollection,
        // kOpenedCollection (nullptr otherwise).
        std::shared_ptr<Collection> collection;

        // True if the collection was created during this transaction for the first time.
        bool newColl;
    };

    UncommittedCatalogUpdates() {}
    ~UncommittedCatalogUpdates() = default;

    /**
     * Determine if an entry is associated with a collection action (as opposed to a view action).
     */
    static bool isCollectionEntry(const Entry& entry) {
        return (entry.action == Entry::Action::kCreatedCollection ||
                entry.action == Entry::Action::kWritableCollection ||
                entry.action == Entry::Action::kRenamedCollection ||
                entry.action == Entry::Action::kDroppedCollection ||
                entry.action == Entry::Action::kRecreatedCollection ||
                entry.action == Entry::Action::kOpenedCollection);
    }

    /**
     * Determine if an entry uses two-phase commit to write into the CollectionCatalog.
     * kCreatedCollection is also committed using two-phase commit but using a separate system and
     * is excluded from this list. kDroppedIndex is covered by kWritableCollection as a writable
     * collection must be used to drop an index.
     */
    static bool isTwoPhaseCommitEntry(const Entry& entry) {
        return (entry.action == Entry::Action::kWritableCollection ||
                entry.action == Entry::Action::kRenamedCollection ||
                entry.action == Entry::Action::kDroppedCollection ||
                entry.action == Entry::Action::kRecreatedCollection);
    }

    /**
     * Lookup of Collection by UUID describing whether this namespace is managed, a managed
     * Collection pointer (may be returned as nullptr, which indicates a drop), and if it was
     * created in this transaction.
     */
    static UncommittedCatalogUpdates::CollectionLookupResult lookupCollection(
        OperationContext* opCtx, UUID uuid);

    /**
     * Lookup of Collection by Namestring describing whether this namespace is managed, a managed
     * Collection pointer (may be returned as nullptr, which indicates a drop), and if it was
     * created in this transaction.
     */
    static CollectionLookupResult lookupCollection(OperationContext* opCtx,
                                                   const NamespaceString& nss);

    boost::optional<const ViewsForDatabase&> getViewsForDatabase(const DatabaseName& dbName) const;

    /**
     * Add collection to entries and register RecoveryUnit preCommitHook to throw a
     * `WriteConflictException` if there is a NamespaceString conflict in the catalog.
     */
    void createCollection(OperationContext* opCtx, std::shared_ptr<Collection> coll);

    /**
     * Wraps 'createCollection' and does not register a preCommitHook in order to defer committing a
     * collection after a collection drop.
     */
    void recreateCollection(OperationContext* opCtx, std::shared_ptr<Collection> coll);

    /**
     * Manage the lifetime of uncommitted writable collection.
     */
    void writableCollection(std::shared_ptr<Collection> collection);

    /**
     * Manage an uncommitted rename, pointer must have made writable first and should exist in entry
     * list.
     */
    void renameCollection(const Collection* collection, const NamespaceString& from);

    /**
     * Manages an uncommitted index entry drop.
     */
    void dropIndex(const NamespaceString& nss,
                   std::shared_ptr<IndexCatalogEntry> indexEntry,
                   bool isDropPending);

    /**
     * Manage an uncommitted collection drop.
     */
    void dropCollection(const Collection* collection, bool isDropPending);

    /**
     * Replace the ViewsForDatabase instance assocated with database `dbName` with `vfdb`. This is
     * the primary low-level write method to alter any information about the views associated with a
     * given database.
     */
    void replaceViewsForDatabase(const DatabaseName& dbName, ViewsForDatabase&& vfdb);

    /**
     * Adds a ResourceID associated with a view namespace, and registers a preCommitHook to do
     * conflict-checking on the view namespace.
     */
    void addView(OperationContext* opCtx, const NamespaceString& nss);

    /**
     * Removes the ResourceID associated with a view namespace.
     */
    void removeView(const NamespaceString& nss);

    /**
     * Manages the lifetime of the collection instance from an earlier point-in-time.
     */
    void openCollection(OperationContext* opCtx, std::shared_ptr<Collection> coll);

    /**
     * Returns all entries without releasing them.
     */
    const std::vector<Entry>& entries() const;

    /**
     * Releases all entries, needs to be done when WriteUnitOfWork commits or rolls back.
     */
    std::vector<Entry> releaseEntries();

    /**
     * The catalog needs to ignore external view changes for its own modifications. This method
     * should be used by DDL operations to prevent op observers from triggering additional catalog
     * operations.
     */
    void setIgnoreExternalViewChanges(const DatabaseName& dbName, bool value);

    /**
     * The catalog needs to ignore external view changes for its own modifications. This method can
     * be used by methods called by op observers (e.g. 'CollectionCatalog::reload()') to distinguish
     * between an external write to 'system.views' and one initiated through the proper view DDL
     * operations.
     */
    bool shouldIgnoreExternalViewChanges(const DatabaseName& dbName) const;

    /**
     * Checks if there is an entry with the nss `nss` and the
     * 'kCreatedCollection'/'kRecreatedCollection' action type.
     */
    static bool isCreatedCollection(OperationContext* opCtx, const NamespaceString& nss);

    bool isEmpty() {
        return _entries.empty();
    }

    static UncommittedCatalogUpdates& get(OperationContext* opCtx);

private:
    /**
     * Adds a created or recreated collection to the entries vector and registers rollback handlers
     * (in addition to a preCommitHook for newly created collections).
     */
    void _createCollection(OperationContext* opCtx,
                           std::shared_ptr<Collection> coll,
                           Entry::Action action);

    /**
     * Store entries in vector, we will do linear search to find what we're looking for but it will
     * be very few entries so it should be fine.
     */
    std::vector<Entry> _entries;

    stdx::unordered_set<DatabaseName> _ignoreExternalViewChanges;
};

}  // namespace mongo
