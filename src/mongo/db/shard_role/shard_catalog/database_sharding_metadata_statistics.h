/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/base/counter.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/modules.h"

namespace mongo {
/**
 * Tracks statistics for work related to authoritative database metadata recovery.
 */
class [[MONGO_MOD_PARENT_PRIVATE]] DatabaseShardingMetadataStatistics {
public:
    void report(BSONObjBuilder& builder) const {
        builder.append("countShardCatalogDatabaseWrites", _countShardCatalogDatabaseWrites.get());
        builder.append("countDatabaseMetadataCacheClears", _countDatabaseMetadataCacheClears.get());
        builder.append("countDatabaseMetadataCacheSets", _countDatabaseMetadataCacheSets.get());
        builder.append("countDatabaseMetadataAccessorAccessTypeChanges",
                       _countDatabaseMetadataAccessorAccessTypeChanges.get());
        builder.append("movePrimariesInProgress", _movePrimariesInProgress.get());
        builder.append("countCreateDatabaseMetadataOplogEntriesApplied",
                       _countCreateDatabaseMetadataOplogEntriesApplied.get());
        builder.append("countDropDatabaseMetadataOplogEntriesApplied",
                       _countDropDatabaseMetadataOplogEntriesApplied.get());
        builder.append("countDbVersionMismatchWaits", _countDbVersionMismatchWaits.get());
        builder.append("totalDbVersionMismatchWaitMillis", _totalDbVersionMismatchWaitMillis.get());
        builder.append("countLocalDatabaseMetadataCommits",
                       _countLocalDatabaseMetadataCommits.get());
        builder.append("countLocalDatabaseMetadataClones", _countLocalDatabaseMetadataClones.get());
        builder.append("countLocalDatabaseMetadataDrops", _countLocalDatabaseMetadataDrops.get());
    }

    void registerShardCatalogDatabaseWrite() {
        _countShardCatalogDatabaseWrites.incrementRelaxed();
    }

    void registerDatabaseMetadataCacheClear() {
        _countDatabaseMetadataCacheClears.incrementRelaxed();
    }

    void registerDatabaseMetadataCacheSet() {
        _countDatabaseMetadataCacheSets.incrementRelaxed();
    }

    void registerMovePrimaryInProgress(bool inProgress) {
        if (inProgress) {
            _movePrimariesInProgress.incrementRelaxed();
        } else {
            _movePrimariesInProgress.decrementRelaxed();
        }
    }

    void registerDatabaseMetadataAccessorAccessTypeChange() {
        _countDatabaseMetadataAccessorAccessTypeChanges.incrementRelaxed();
    }

    void registerCreateDatabaseMetadataOplogEntryApplied() {
        _countCreateDatabaseMetadataOplogEntriesApplied.incrementRelaxed();
    }

    void registerDropDatabaseMetadataOplogEntryApplied() {
        _countDropDatabaseMetadataOplogEntriesApplied.incrementRelaxed();
    }

    void registerDbVersionMismatchWait(long long millis) {
        _countDbVersionMismatchWaits.incrementRelaxed();
        _totalDbVersionMismatchWaitMillis.incrementRelaxed(millis);
    }

    void registerLocalDatabaseMetadataCommit() {
        _countLocalDatabaseMetadataCommits.incrementRelaxed();
    }

    void registerLocalDatabaseMetadataClone() {
        _countLocalDatabaseMetadataClones.incrementRelaxed();
    }

    void registerLocalDatabaseMetadataDrop() {
        _countLocalDatabaseMetadataDrops.incrementRelaxed();
    }

private:
    // Persisted insert/update/delete writes to config.shard.catalog.databases.
    Counter64 _countShardCatalogDatabaseWrites;
    // In-memory clears of the per-database metadata cache (DSS).
    Counter64 _countDatabaseMetadataCacheClears;
    // In-memory sets of the per-database metadata cache (DSS).
    Counter64 _countDatabaseMetadataCacheSets;
    // Databases with movePrimary currently marked in-progress (gauge, not cumulative).
    Counter64 _movePrimariesInProgress;
    // Transitions between read and write access on database metadata accessors.
    Counter64 _countDatabaseMetadataAccessorAccessTypeChanges;
    // createDatabaseMetadata ('c') oplog entries applied on replication secondaries.
    Counter64 _countCreateDatabaseMetadataOplogEntriesApplied;
    // dropDatabaseMetadata ('c') oplog entries applied on replication secondaries.
    Counter64 _countDropDatabaseMetadataOplogEntriesApplied;
    // Waits for majority replication of a received database version on mismatch.
    Counter64 _countDbVersionMismatchWaits;
    // Wall-clock time spent waiting on dbVersion mismatches.
    Counter64 _totalDbVersionMismatchWaitMillis;
    // Local shard-catalog database commits (createDatabase / movePrimary / drop recreation).
    Counter64 _countLocalDatabaseMetadataCommits;
    // Local shard-catalog database clones during FCV upgrade (cloneAuthoritativeMetadata).
    Counter64 _countLocalDatabaseMetadataClones;
    // Local shard-catalog database drops.
    Counter64 _countLocalDatabaseMetadataDrops;
};
}  // namespace mongo
