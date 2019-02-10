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
 * Writes the 'indexBuildEntry' to the disk.
 *
 * An IndexBuildEntry should be stored on the disk during the duration of the index build process
 * for the 'indexBuildEntry'.
 *
 * Returns 'DuplicateKey' error code if a document already exists on the disk with the same
 * 'indexBuildUUID'.
 */
Status addIndexBuildEntry(OperationContext* opCtx, IndexBuildEntry indexBuildEntry);

/**
 * Removes the IndexBuildEntry from the disk.
 *
 * An IndexBuildEntry should be removed from the disk when the index build either succeeds or fails
 * for the given 'indexBuildUUID'.
 *
 * Returns 'NoMatchingDocument' error code if no document with 'indexBuildUUID' is found.
 */
Status removeIndexBuildEntry(OperationContext* opCtx, UUID indexBuildUUID);

/**
 * Returns the IndexBuildEntry matching the document with 'indexBuildUUID' from the disk if it
 * exists.
 *
 * If the stored IndexBuildEntry on disk contains invalid BSON, the 'InvalidBSON' error code is
 * returned.
 *
 * Returns 'NoMatchingDocument' error code if no document with 'indexBuildUUID' is found.
 */
StatusWith<IndexBuildEntry> getIndexBuildEntry(OperationContext* opCtx, UUID indexBuildUUID);

/**
 * Returns a vector of matching IndexBuildEntries matching the documents with 'collectionUUID'
 * from disk.
 *
 * Can be used to get all the unfinished index builds on the collection if the indexBuildUUID is
 * unknown.
 */
StatusWith<std::vector<IndexBuildEntry>> getIndexBuildEntries(OperationContext* opCtx,
                                                              UUID collectionUUID);

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
 */
Status setCommitQuorum(OperationContext* opCtx,
                       UUID indexBuildUUID,
                       CommitQuorumOptions commitQuorumOptions);

/**
 * Adds 'hostAndPort' to the 'commitReadyMembers' field for the document with 'indexBuildUUID'.
 * If the 'hostAndPort' is already in the 'commitReadyMembers' field, nothing is done.
 *
 * When a replica set member is ready to commit the index build, we need to record this.
 *
 * Returns 'NoMatchingDocument' error code if no document with 'indexBuildUUID' is found.
 */
Status addCommitReadyMember(OperationContext* opCtx, UUID indexBuildUUID, HostAndPort hostAndPort);

/**
 * Removes 'hostAndPort' from the 'commitReadyMembers' field for the document with
 * 'indexBuildUUID' if it exists.
 *
 * If a replica set member is removed during a reconfig and it was a commit ready member, we need to
 * remove its entry from the 'commitReadyMembers' field.
 *
 * Returns 'NoMatchingDocument' error code if no document with 'indexBuildUUID' is found.
 */
Status removeCommitReadyMember(OperationContext* opCtx,
                               UUID indexBuildUUID,
                               HostAndPort hostAndPort);

/**
 * Returns a vector of HostAndPorts of all the 'commitReadyMembers' for the document with
 * 'indexBuildUUID'.
 *
 * Returns 'NoMatchingDocument' error code if no document with 'indexBuildUUID' is found.
 */
StatusWith<std::vector<HostAndPort>> getCommitReadyMembers(OperationContext* opCtx,
                                                           UUID indexBuildUUID);

/**
 * Truncates all the documents in the "config.system.indexBuilds" collection.
 * Can be used during recovery to remove unfinished index builds to restart them.
 */
Status clearAllIndexBuildEntries(OperationContext* opCtx);

}  // namespace indexbuildentryhelpers
}  // namespace mongo
