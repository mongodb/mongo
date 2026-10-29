// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/index_builds/index_build_entry_gen.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/util/modules.h"

/**
 * Format of IndexBuildEntry:
 * {
 *		_id : indexBuildUUID,
 *		collectionUUID : <UUID>,
 *		commitQuorum : <BSON>,
 *		indexes : [<index_name1>, <index_name2>, ...],
 *		commitReadyMembers : [
 *			<hostAndPort1>,
 *			<hostAndPort2>,
 *			...
 *		]
 *	}
 */

namespace mongo::indexbuildentryhelpers {

/**
 * Creates the "config.system.indexBuilds" collection if it does not already exist.
 * This is the collection where the IndexBuildEntries will be stored on disk for active index
 * builds.
 *
 * The collection should exist before calling any other helper functions to prevent them from
 * failing.
 */
[[MONGO_MOD_PUBLIC]] void ensureIndexBuildEntriesNamespaceExists(OperationContext* opCtx);

/**
 * Persist the host and port information about the replica set members that have voted to commit an
 * index build into config.system.indexBuilds collection. If the member info is already present in
 * the collection for that index build, then we don't do any updates and don't generate any errors.
 *
 * Returns an error if collection is missing.
 */
Status persistCommitReadyMemberInfo(OperationContext* opCtx,
                                    const IndexBuildEntry& indexBuildEntry);

/**
 * Persist the new commit quorum value for an index build into config.system.indexBuilds collection.
 *
 * Returns an error if collection is missing.
 */
Status persistIndexCommitQuorum(OperationContext* opCtx, const IndexBuildEntry& indexBuildEntry);

/**
 * Writes the 'indexBuildEntry' to the disk.
 *
 * An IndexBuildEntry should be stored on the disk during the duration of the index build process
 * for the 'indexBuildEntry'.
 *
 * Returns 'DuplicateKey' error code if a document already exists on the disk with the same
 * 'indexBuildUUID'.
 */
Status addIndexBuildEntry(OperationContext* opCtx, const IndexBuildEntry& indexBuildEntry);

/**
 * Removes the IndexBuildEntry from the disk.
 *
 * An IndexBuildEntry should be removed from the disk when the index build either succeeds or fails
 * for the given 'indexBuildUUID'.
 *
 * Returns 'NoMatchingDocument' error code if no document with 'indexBuildUUID' is found.
 */
Status removeIndexBuildEntry(OperationContext* opCtx,
                             const CollectionPtr& collection,
                             UUID indexBuildUUID);

/**
 * Returns the IndexBuildEntry matching the document with 'indexBuildUUID' from the disk if it
 * exists. Reads at "no" timestamp i.e, reading with the "latest" snapshot reflecting up to date
 * data.
 *
 * If the stored IndexBuildEntry on disk contains invalid BSON, the 'InvalidBSON' error code is
 * returned.
 *
 * Returns 'NoMatchingDocument' error code if no document with 'indexBuildUUID' is found.
 */
StatusWith<IndexBuildEntry> getIndexBuildEntry(OperationContext* opCtx, UUID indexBuildUUID);

/**
 * Returns the 'commitQuorum' matching the document with 'indexBuildUUID' from disk if it
 * exists.
 *
 * Returns 'NoMatchingDocument' error code if no document with 'indexBuildUUID' is found.
 */
StatusWith<CommitQuorumOptions> getCommitQuorum(OperationContext* opCtx, UUID indexBuildUUID);

/**
 * Sets the documents 'commitQuorum' field matching the document with 'indexBuildUUID'.
 *
 * Since the commit quorum is configurable until the index build is committed, this should be called
 * whenever the commit quorum is changed.
 *
 * Returns 'NoMatchingDocument' error code if no document with 'indexBuildUUID' is found.
 *
 * Used for testing only.
 */
Status setCommitQuorum_forTest(OperationContext* opCtx,
                               UUID indexBuildUUID,
                               CommitQuorumOptions commitQuorumOptions);

}  // namespace mongo::indexbuildentryhelpers
