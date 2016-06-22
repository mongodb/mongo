/**
 *    Copyright (C) 2015 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <memory>
#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/string_data.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/s/metadata_manager.h"

namespace mongo {

class BSONObj;
struct ChunkVersion;
class CollectionMetadata;
class MigrationSourceManager;
class OperationContext;

/**
 * Contains all sharding-related runtime state for a given collection. One such object is assigned
 * to each sharded collection known on a mongod instance. A set of these objects is linked off the
 * instance's sharding state.
 *
 * Synchronization rules: In order to look-up this object in the instance's sharding map, one must
 * have some lock on the respective collection.
 */
class CollectionShardingState {
    MONGO_DISALLOW_COPYING(CollectionShardingState);

public:
    /**
     * Instantiates a new per-collection sharding state with some initial metadata.
     */
    CollectionShardingState(NamespaceString nss,
                            std::unique_ptr<CollectionMetadata> initialMetadata);
    ~CollectionShardingState();

    /**
     * Obtains the sharding state for the specified collection. If it does not exist, it will be
     * created and will remain active until the collection is dropped or unsharded.
     *
     * Must be called with some lock held on the specific collection being looked up and the
     * returned pointer should never be stored.
     */
    static CollectionShardingState* get(OperationContext* txn, const NamespaceString& nss);
    static CollectionShardingState* get(OperationContext* txn, const std::string& ns);

    /**
     * Returns the chunk metadata for the collection.
     */
    ScopedCollectionMetadata getMetadata();

    /**
     * Set a new metadata to be used for this collection.
     */
    void setMetadata(std::unique_ptr<CollectionMetadata> newMetadata);

    /**
     * Returns the active migration source manager, if one is available.
     */
    MigrationSourceManager* getMigrationSourceManager();

    /**
     * Attaches a migration source manager to this collection's sharding state. Must be called with
     * collection X lock. May not be called if there is a migration source manager already
     * installed. Must be followed by a call to clearMigrationSourceManager.
     */
    void setMigrationSourceManager(OperationContext* txn, MigrationSourceManager* sourceMgr);

    /**
     * Removes a migration source manager from this collection's sharding state. Must be called with
     * collection X lock. May not be called if there isn't a migration source manager installed
     * already through a previous call to setMigrationSourceManager.
     */
    void clearMigrationSourceManager(OperationContext* txn);

    /**
     * Checks whether the shard version in the context is compatible with the shard version of the
     * collection locally and if not throws SendStaleConfigException populated with the expected and
     * actual versions.
     *
     * Because SendStaleConfigException has special semantics in terms of how a sharded command's
     * response is constructed, this function should be the only means of checking for shard version
     * match.
     */
    void checkShardVersionOrThrow(OperationContext* txn);

    // Replication subsystem hooks. If this collection is serving as a source for migration, these
    // methods inform it of any changes to its contents.

    bool isDocumentInMigratingChunk(OperationContext* txn, const BSONObj& doc);

    void onInsertOp(OperationContext* txn, const BSONObj& insertedDoc);

    void onUpdateOp(OperationContext* txn, const BSONObj& updatedDoc);

    void onDeleteOp(OperationContext* txn, const BSONObj& deletedDocId);

private:
    /**
     * Checks whether the shard version of the operation matches that of the collection.
     *
     * txn - Operation context from which to retrieve the operation's expected version.
     * errmsg (out) - On false return contains an explanatory error message.
     * expectedShardVersion (out) - On false return contains the expected collection version on this
     *  shard. Obtained from the operation sharding state.
     * actualShardVersion (out) - On false return contains the actual collection version on this
     *  shard. Obtained from the collection sharding state.
     *
     * Returns true if the expected collection version on the shard matches its actual version on
     * the shard and false otherwise. Upon false return, the output parameters will be set.
     */
    bool _checkShardVersionOk(OperationContext* txn,
                              std::string* errmsg,
                              ChunkVersion* expectedShardVersion,
                              ChunkVersion* actualShardVersion);

    // Namespace to which this state belongs.
    const NamespaceString _nss;

    MetadataManager _metadataManager;

    // If this collection is serving as a source shard for chunk migration, this value will be
    // non-null. To write this value there needs to be X-lock on the collection in order to
    // synchronize with other callers, which read it.
    //
    // NOTE: The value is not owned by this class.
    MigrationSourceManager* _sourceMgr{nullptr};
};

}  // namespace mongo
