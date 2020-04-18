/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include <vector>

namespace mongo {

class IndexBuildEntry;
class CommitQuorumOptions;
class OperationContext;
class Status;
template <typename T>
class StatusWith;
class UUID;
struct HostAndPort;

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

namespace indexbuildentryhelpers {

/**
 * Creates the "config.system.indexBuilds" collection if it does not already exist.
 * This is the collection where the IndexBuildEntries will be stored on disk for active index
 * builds.
 *
 * The collection should exist before calling any other helper functions to prevent them from
 * failing.
 */
void ensureIndexBuildEntriesNamespaceExists(OperationContext* opCtx);

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
 *
 * TODO(SERVER-47635): use this to remove index build entries for the finished index builds from
 * system.indexBuilds collection.
 */
Status removeIndexBuildEntry(OperationContext* opCtx, UUID indexBuildUUID);

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

}  // namespace indexbuildentryhelpers
}  // namespace mongo
