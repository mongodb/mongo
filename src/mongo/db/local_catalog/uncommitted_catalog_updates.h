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

#include "mongo/db/database_name.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/local_catalog/index_catalog_entry.h"
#include "mongo/db/local_catalog/views_for_database.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/views/view.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <vector>

#include <boost/container/small_vector.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * Decoration on Snapshot to store cloned Collections until they are committed or rolled back.
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
        };

        boost::optional<UUID> uuid() const {
            if (action == Action::kCreatedCollection || action == Action::kWritableCollection ||
                action == Action::kRenamedCollection)
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
    };

    struct CollectionLookupResult {
        // True if the collection is currently being managed in this transaction.
        bool found;

        // Storage for the actual collection.
        // Set for actions kWritableCollection, kCreatedCollection, and kRecreatedCollection.
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
                entry.action == Entry::Action::kRecreatedCollection);
    }

    /**
     * Determine if an entry uses two-phase commit to write into the CollectionCatalog.
     * kCreatedCollection is also committed using two-phase commit but using a separate system and
     * is excluded from this list.
     */
    static bool isTwoPhaseCommitEntry(const Entry& entry) {
        return (entry.action == Entry::Action::kCreatedCollection ||
                entry.action == Entry::Action::kWritableCollection ||
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
     * Manage an uncommitted collection drop.
     */
    void dropCollection(const Collection* collection);

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

    /**
     * Flag to check of callbacks with the RecoveryUnit has been registered for this instance.
     */
    bool hasRegisteredWithRecoveryUnit() const {
        return _callbacksRegisteredWithRecoveryUnit;
    }

    /**
     * Mark that callbacks with the RecoveryUnit has been registered for this instance.
     */
    void markRegisteredWithRecoveryUnit() {
        invariant(!_callbacksRegisteredWithRecoveryUnit);
        _callbacksRegisteredWithRecoveryUnit = true;
    }

    /**
     * Flag to check if precommit has executed successfully and all uncommitted collections have
     * been registered as pending commit
     */
    bool hasPrecommitted() const {
        return _preCommitted;
    }

    /**
     * Mark that precommit has executed successfully and all uncommitted collections have
     * been registered as pending commit
     */
    void markPrecommitted() {
        invariant(!_preCommitted);
        _preCommitted = true;
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

    bool _callbacksRegisteredWithRecoveryUnit = false;
    bool _preCommitted = false;
};

/**
 * Decoration on Snapshot to store Collections instantiated from durable catalog data. Lifetime tied
 * to Snapshot lifetime.
 */
class OpenedCollections {
public:
    static OpenedCollections& get(OperationContext* opCtx);

    /**
     * Lookup collection instance by namespace.
     *
     * May return nullptr which indicates that the namespace does not exist in the snapshot.
     *
     * Returns boost::none if this namespace is unknown to OpenedCollections.
     */
    boost::optional<std::shared_ptr<const Collection>> lookupByNamespace(
        const NamespaceString& ns) const;

    /**
     * Lookup collection instance by UUID.
     *
     * May return nullptr which indicates that the UUID does not exist in the snapshot.
     *
     * Returns boost::none if this UUID is unknown to OpenedCollections.
     */
    boost::optional<std::shared_ptr<const Collection>> lookupByUUID(UUID uuid) const;

    /**
     * Stores a Collection instance. Lifetime of instance will be tied to lifetime of opened storage
     * snapshot.
     *
     * Collection instance may be nullptr to indicate that the namespace and/or UUID does not exist
     * in the snapshot.
     */
    void store(std::shared_ptr<const Collection> coll,
               boost::optional<NamespaceString> nss,
               boost::optional<UUID> uuid);

private:
    struct Entry {
        std::shared_ptr<const Collection> collection;
        // TODO(SERVER-78226): Replace `nss` and `uuid` with a type which can express "nss and uuid"
        boost::optional<NamespaceString> nss;
        boost::optional<UUID> uuid;
    };

    // Static storage for one entry. The expected common case is that only a single collection will
    // be needed so we optimize for that.
    boost::container::small_vector<Entry, 1> _collections;
};

}  // namespace mongo
