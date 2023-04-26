/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/namespace_string.h"
#include "mongo/db/record_id.h"
#include "mongo/util/immutable/unordered_map.h"
#include "mongo/util/immutable/unordered_set.h"
#include "mongo/util/uuid.h"

namespace mongo {
/**
 * Data structure to keep track of mappings between namespace or uuid to a catalogId. The mapping is
 * maintained for a range of time [oldest, now) to mirror the time range the server can service
 * queries.
 *
 * Uses immutable data structures internally to be cheap to copy.
 */
class HistoricalCatalogIdTracker {
public:
    HistoricalCatalogIdTracker(Timestamp oldest = Timestamp::max())
        : _oldestTimestampMaintained(oldest) {}

    /**
     * CatalogId with Timestamp
     */
    struct TimestampedCatalogId {
        boost::optional<RecordId>
            id;  // none represents non-existing at timestamp (due to drop or rename)
        Timestamp ts;
    };

    /**
     * Returns the CatalogId for a given 'nss' or 'uuid' at timestamp 'ts'.
     */
    struct LookupResult {
        enum class Existence {
            // Namespace or UUID exists at time 'ts' and catalogId set in 'id'.
            kExists,
            // Namespace or UUID does not exist at time 'ts'.
            kNotExists,
            // Namespace or UUID existence at time 'ts' is unknown. The durable catalog must be
            // scanned to determine existence.
            kUnknown
        };
        RecordId id;
        Existence result;
    };

    /**
     * Returns the CatalogId for a given 'nss' or 'uuid' at timestamp 'ts'.
     *
     * Timestamp 'none' returns mapping at latest.
     */
    LookupResult lookup(const NamespaceString& nss, boost::optional<Timestamp> ts) const;
    LookupResult lookup(const UUID& uuid, boost::optional<Timestamp> ts) const;

    /**
     * Register that a namespace/uuid was created with given 'catalogId' at timestamp 'ts'.
     *
     * Timestamp 'none' indicates that the namespace was created without a timestamp.
     */
    void create(const NamespaceString& nss,
                const UUID& uuid,
                RecordId catalogId,
                boost::optional<Timestamp> ts);

    /**
     * Register that a namespace/uuid was dropped at timestamp 'ts'.
     *
     * Timestamp 'none' indicates that the namespace was dropped without a timestamp.
     */
    void drop(const NamespaceString& nss, const UUID& uuid, boost::optional<Timestamp> ts);

    /**
     * Register that a namespace was renamed at timestamp 'ts'.
     *
     * Timestamp 'none' indicates that the namespace was renamed without a timestamp.
     */
    void rename(const NamespaceString& from,
                const NamespaceString& to,
                boost::optional<Timestamp> ts);

    /**
     * Records existence of a namespace at timestamp 'ts' that was previously unknown.
     */
    void recordExistingAtTime(const NamespaceString& nss,
                              const UUID& uuid,
                              RecordId catalogId,
                              Timestamp ts);

    /**
     * Records non-existence of a namespace at timestamp 'ts' that was previously unknown.
     */
    void recordNonExistingAtTime(const NamespaceString& nss, Timestamp ts);
    void recordNonExistingAtTime(const UUID& uuid, Timestamp ts);

    /**
     * Returns true if the structure has space to record non-existence of a namespace/uuid.
     */
    bool canRecordNonExisting(const NamespaceString& nss) const;
    bool canRecordNonExisting(const UUID& uuid) const;

    /**
     * Returns true if a call to 'cleanup' with the given timestemp would perform any cleanup.
     */
    bool dirty(Timestamp oldest) const;

    /**
     * Performs cleanup of historical data when the oldest timestamp advances. Should be performed
     * regularly to free up data for time ranges that are no longer needed for lookups.
     */
    void cleanup(Timestamp oldest);

    /**
     * Rollback any mappings with larger timestamps than provided stable timestamp. Needs to be
     * performed as part of replication rollback.
     */
    void rollback(Timestamp stable);

private:
    void _recordCleanupTime(Timestamp ts);

    void _createTimestamp(const NamespaceString& nss,
                          const UUID& uuid,
                          RecordId catalogId,
                          Timestamp ts);
    void _createNoTimestamp(const NamespaceString& nss, const UUID& uuid, RecordId catalogId);
    void _dropTimestamp(const NamespaceString& nss, const UUID& uuid, Timestamp ts);
    void _dropNoTimestamp(const NamespaceString& nss, const UUID& uuid);
    void _renameTimestamp(const NamespaceString& from, const NamespaceString& to, Timestamp ts);
    void _renameNoTimestamp(const NamespaceString& from, const NamespaceString& to);


    // CatalogId mappings for all known namespaces and UUIDs for the CollectionCatalog. The vector
    // is sorted on timestamp. UUIDs will have at most two entries. One for the create and another
    // for the drop. UUIDs stay the same across collection renames.
    immutable::unordered_map<NamespaceString, std::vector<TimestampedCatalogId>> _nss;
    immutable::unordered_map<UUID, std::vector<TimestampedCatalogId>, UUID::Hash> _uuid;
    // Set of namespaces and UUIDs that need cleanup when the oldest timestamp advances
    // sufficiently.
    immutable::unordered_set<NamespaceString> _nssChanges;
    immutable::unordered_set<UUID, UUID::Hash> _uuidChanges;
    // Point at which the oldest timestamp need to advance for there to be any catalogId namespace
    // that can be cleaned up
    Timestamp _lowestTimestampForCleanup = Timestamp::max();
    // The oldest timestamp at which the tracker maintains mappings. Anything older than this is
    // unknown.
    Timestamp _oldestTimestampMaintained;
};

}  // namespace mongo
