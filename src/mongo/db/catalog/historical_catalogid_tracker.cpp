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

#include "mongo/db/catalog/historical_catalogid_tracker.h"
#include "mongo/db/storage/storage_options.h"

namespace mongo {
namespace {
// Sentinel id for marking a catalogId mapping range as unknown. Must use an invalid RecordId.
static RecordId kUnknownRangeMarkerId = RecordId::minLong();
// Maximum number of entries in catalogId mapping when inserting catalogId missing at timestamp.
// Used to avoid quadratic behavior when inserting entries at the beginning. When threshold is
// reached we will fall back to more durable catalog scans.
static constexpr int kMaxCatalogIdMappingLengthForMissingInsert = 1000;

// Copy existing value from immutable data structure or default-construct if not existing
template <class Container, class Key>
auto copyIfExists(const Container& container, const Key& key) {
    const auto* value = container.find(key);
    if (value) {
        return *value;
    }
    return typename Container::mapped_type();
}

// Returns true if cleanup is needed for a catalogId range
bool needsCleanup(const std::vector<HistoricalCatalogIdTracker::TimestampedCatalogId>& ids) {
    // Cleanup may occur if we have more than one entry for the namespace.
    return ids.size() > 1;
}

// Returns the lowest time a catalogId range may be cleaned up. needsCleanup() needs to have been
// checked prior to calling this function
Timestamp cleanupTime(const std::vector<HistoricalCatalogIdTracker::TimestampedCatalogId>& ids) {
    // When we have multiple entries, use the time at the second entry as the cleanup time,
    // when the oldest timestamp advances past this we no longer need the first entry.
    return ids.at(1).ts;
}

// Converts a not found lookup timestamp to a LookupResult based on the oldest maintained timestamp
HistoricalCatalogIdTracker::LookupResult resultForNotFound(boost::optional<Timestamp> ts,
                                                           Timestamp oldestMaintained) {
    // If the request was with a time prior to the oldest maintained time it is unknown, otherwise
    // we know it is not existing.
    return {RecordId{},
            ts && *ts < oldestMaintained
                ? HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown
                : HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists};
}

// Converts a catalogId range into a lookup result that represents the latest state
HistoricalCatalogIdTracker::LookupResult latestInRange(
    const std::vector<HistoricalCatalogIdTracker::TimestampedCatalogId>& range) {
    auto catalogId = range.back().id;
    if (catalogId) {
        return {*catalogId, HistoricalCatalogIdTracker::LookupResult::Existence::kExists};
    }
    return {RecordId{}, HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists};
}

HistoricalCatalogIdTracker::LookupResult findInRange(
    Timestamp ts,
    const std::vector<HistoricalCatalogIdTracker::TimestampedCatalogId>& range,
    Timestamp oldestMaintained) {
    // The algorithm is as follows for an input range of the following format that is sorted on
    // timestamp: (ts1, id1), (ts2, id2), ..., (tsN, idN).
    //
    // We use upper_bound to perform binary search to the timestamp that is strictly larger than our
    // query timestamp ts. The iterator can then be decremented to get the entry where the time is
    // less or equal, this is the entry we are looking for. If upper_bound returns begin() or the
    // 'id' in our found entry is the unknown marker the lookup result is unknown.
    auto rangeIt =
        std::upper_bound(range.begin(), range.end(), ts, [](const auto& ts, const auto& entry) {
            return ts < entry.ts;
        });
    if (rangeIt == range.begin()) {
        return resultForNotFound(ts, oldestMaintained);
    }
    // Upper bound returns an iterator to the first entry with a larger timestamp. Decrement the
    // iterator to get the last entry where the time is less or equal.
    auto catalogId = (--rangeIt)->id;
    if (catalogId) {
        if (*catalogId != kUnknownRangeMarkerId) {
            return {*catalogId, HistoricalCatalogIdTracker::LookupResult::Existence::kExists};
        } else {
            return {RecordId{}, HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown};
        }
    }
    return {RecordId{}, HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists};
}
}  // namespace

HistoricalCatalogIdTracker::LookupResult HistoricalCatalogIdTracker::lookup(
    const NamespaceString& nss, boost::optional<Timestamp> ts) const {
    if (const std::vector<TimestampedCatalogId>* mapping = _nss.find(nss)) {
        // Mapping found for namespace, get result depending on timestamp.
        if (ts) {
            return findInRange(*ts, *mapping, _oldestTimestampMaintained);
        }
        return latestInRange(*mapping);
    }
    // No mapping found for namespace, result is either not found or unknown depending on timestamp
    return resultForNotFound(ts, _oldestTimestampMaintained);
}


HistoricalCatalogIdTracker::LookupResult HistoricalCatalogIdTracker::lookup(
    const UUID& uuid, boost::optional<Timestamp> ts) const {
    if (const std::vector<TimestampedCatalogId>* mapping = _uuid.find(uuid)) {
        // Mapping found for namespace, get result depending on timestamp.
        if (ts) {
            return findInRange(*ts, *mapping, _oldestTimestampMaintained);
        }
        return latestInRange(*mapping);
    }

    // No mapping found for namespace, result is either not found or unknown depending on timestamp
    return resultForNotFound(ts, _oldestTimestampMaintained);
}

void HistoricalCatalogIdTracker::create(const NamespaceString& nss,
                                        const UUID& uuid,
                                        RecordId catalogId,
                                        boost::optional<Timestamp> ts) {

    if (!ts) {
        _createNoTimestamp(nss, uuid, catalogId);
        return;
    }

    _createTimestamp(nss, uuid, catalogId, *ts);
}

void HistoricalCatalogIdTracker::drop(const NamespaceString& nss,
                                      const UUID& uuid,
                                      boost::optional<Timestamp> ts) {
    if (!ts) {
        _dropNoTimestamp(nss, uuid);
        return;
    }

    _dropTimestamp(nss, uuid, *ts);
}

void HistoricalCatalogIdTracker::rename(const NamespaceString& from,
                                        const NamespaceString& to,
                                        boost::optional<Timestamp> ts) {
    if (!ts) {
        _renameNoTimestamp(from, to);
        return;
    }

    _renameTimestamp(from, to, *ts);
}

bool HistoricalCatalogIdTracker::canRecordNonExisting(const NamespaceString& nss) const {
    // recordNonExistingAtTime can use a lot of entries because of the unknown marker that is
    // needed. Constrain the memory usage.
    if (const std::vector<TimestampedCatalogId>* ids = _nss.find(nss)) {
        return ids->size() < kMaxCatalogIdMappingLengthForMissingInsert;
    }
    return true;
}

bool HistoricalCatalogIdTracker::canRecordNonExisting(const UUID& uuid) const {
    // recordNonExistingAtTime can use a lot of entries because of the unknown marker that is
    // needed. Constrain the memory usage.
    if (const std::vector<TimestampedCatalogId>* ids = _uuid.find(uuid)) {
        return ids->size() < kMaxCatalogIdMappingLengthForMissingInsert;
    }
    return true;
}

void HistoricalCatalogIdTracker::recordExistingAtTime(const NamespaceString& nss,
                                                      const UUID& uuid,
                                                      RecordId catalogId,
                                                      Timestamp ts) {

    // Helper lambda to perform the operation on both namespace and UUID
    auto doRecord =
        [this, &catalogId, &ts](auto& idsContainer, auto& changesContainer, const auto& key) {
            // Helper to update the cleanup time after we've performed an insert.
            auto markForCleanupIfNeeded = [&](const auto& ids) {
                if (!needsCleanup(ids)) {
                    return;
                }

                changesContainer = changesContainer.insert(key);
                _recordCleanupTime(cleanupTime(ids));
            };

            // Get copy of existing mapping, or default-construct new.
            auto ids = copyIfExists(idsContainer, key);
            // Helper to write updated id mapping back into container at scope exit. This allows us
            // to write to 'ids' as if we were doing inplace updates to the container.
            ScopeGuard scopedGuard([&] { idsContainer = idsContainer.set(key, std::move(ids)); });

            // Binary search to the entry with same or larger timestamp. This represents the insert
            // position in the container.
            auto it = std::lower_bound(
                ids.begin(), ids.end(), ts, [](const auto& entry, const Timestamp& ts) {
                    return entry.ts < ts;
                });

            if (it != ids.end()) {
                // An entry could exist already if concurrent writes are performed, keep the latest
                // change in that case.
                if (it->ts == ts) {
                    it->id = catalogId;
                    return;
                }

                // If next element has same catalogId, we can adjust its timestamp to cover a longer
                // range
                if (it->id == catalogId) {
                    it->ts = ts;

                    markForCleanupIfNeeded(ids);
                    return;
                }
            }

            // Otherwise insert new entry at timestamp
            ids.insert(it, {{catalogId, ts}});
            markForCleanupIfNeeded(ids);
        };

    // Apply the insert to both namespace and uuid.
    doRecord(_nss, _nssChanges, nss);
    doRecord(_uuid, _uuidChanges, uuid);
}

void HistoricalCatalogIdTracker::recordNonExistingAtTime(const NamespaceString& nss, Timestamp ts) {
    // Get copy of existing mapping, or default-construct new.
    auto ids = copyIfExists(_nss, nss);

    // Avoid inserting missing mapping when the list has grown past the threshold. Will cause
    // the system to fall back to scanning the durable catalog.
    if (ids.size() >= kMaxCatalogIdMappingLengthForMissingInsert) {
        return;
    }

    // Helper to write updated id mapping back into container at scope exit
    ScopeGuard scopedGuard([&] { _nss = _nss.set(nss, std::move(ids)); });

    // Binary search to the entry with same or larger timestamp. This represents the insert position
    // in the container.
    auto it =
        std::lower_bound(ids.begin(), ids.end(), ts, [](const auto& entry, const Timestamp& ts) {
            return entry.ts < ts;
        });

    if (it != ids.end() && it->ts == ts) {
        // An entry could exist already if concurrent writes are performed, keep the latest
        // change in that case.
        it->id = boost::none;
    } else {
        // Otherwise insert new entry
        it = ids.insert(it, {boost::none, ts});
    }

    // The iterator is positioned on the added/modified element above, reposition it to the next
    // entry
    ++it;

    // We don't want to assume that the namespace remains not existing until the next entry, as
    // there can be times where the namespace actually does exist. To make sure we trigger the
    // scanning of the durable catalog in this range we will insert a bogus entry using an invalid
    // RecordId at the next timestamp. This will treat the range forward as unknown.
    auto nextTs = ts + 1;

    // If the next entry is on the next timestamp already, we can skip adding the bogus entry.
    // If this function is called for a previously unknown namespace or UUID, we may not have
    // any future valid entries and the iterator would be positioned at and at this point.
    if (it == ids.end() || it->ts != nextTs) {
        ids.insert(it, {kUnknownRangeMarkerId, nextTs});
    }

    // Update cleanup time if needed
    if (!needsCleanup(ids)) {
        return;
    }

    _nssChanges = _nssChanges.insert(nss);
    _recordCleanupTime(cleanupTime(ids));
}

void HistoricalCatalogIdTracker::recordNonExistingAtTime(const UUID& uuid, Timestamp ts) {
    auto ids = copyIfExists(_uuid, uuid);

    // Avoid inserting missing mapping when the list has grown past the threshold. Will cause
    // the system to fall back to scanning the durable catalog.
    if (ids.size() >= kMaxCatalogIdMappingLengthForMissingInsert) {
        return;
    }

    // Helper to write updated id mapping back into container at scope exit
    ScopeGuard scopedGuard([&] { _uuid = _uuid.set(uuid, std::move(ids)); });

    // Binary search to the entry with same or larger timestamp. This represents the insert position
    // in the container.
    auto it =
        std::lower_bound(ids.begin(), ids.end(), ts, [](const auto& entry, const Timestamp& ts) {
            return entry.ts < ts;
        });

    if (it != ids.end() && it->ts == ts) {
        // An entry could exist already if concurrent writes are performed, keep the latest
        // change in that case.
        it->id = boost::none;
    } else {
        // Otherwise insert new entry
        it = ids.insert(it, {boost::none, ts});
    }

    // The iterator is positioned on the added/modified element above, reposition it to the next
    // entry
    ++it;

    // We don't want to assume that the namespace remains not existing until the next entry, as
    // there can be times where the namespace actually does exist. To make sure we trigger the
    // scanning of the durable catalog in this range we will insert a bogus entry using an invalid
    // RecordId at the next timestamp. This will treat the range forward as unknown.
    auto nextTs = ts + 1;

    // If the next entry is on the next timestamp already, we can skip adding the bogus entry.
    // If this function is called for a previously unknown namespace or UUID, we may not have
    // any future valid entries and the iterator would be positioned at and at this point.
    if (it == ids.end() || it->ts != nextTs) {
        ids.insert(it, {kUnknownRangeMarkerId, nextTs});
    }

    // Update cleanup time if needed
    if (!needsCleanup(ids)) {
        return;
    }

    _uuidChanges = _uuidChanges.insert(uuid);
    _recordCleanupTime(cleanupTime(ids));
}

bool HistoricalCatalogIdTracker::dirty(Timestamp oldest) const {
    return _lowestTimestampForCleanup <= oldest;
}

void HistoricalCatalogIdTracker::cleanup(Timestamp oldest) {
    Timestamp nextLowestCleanupTimestamp = Timestamp::max();

    // Helper lambda to perform the operation on both namespace and UUID
    auto doCleanup = [this, &oldest, &nextLowestCleanupTimestamp](auto& idsContainer,
                                                                  auto& changesContainer) {
        // Batch all changes together
        auto ids = idsContainer.transient();
        auto changes = changesContainer.transient();

        for (auto&& key : changesContainer) {
            //
            auto range = ids.at(key);

            // Binary search for next larger timestamp
            auto rangeIt = std::upper_bound(
                range.begin(), range.end(), oldest, [](const auto& ts, const auto& entry) {
                    return ts < entry.ts;
                });

            // Continue if there is nothing to cleanup for this timestamp yet
            if (rangeIt == range.begin()) {
                // There should always be at least two entries in the range when we hit this
                // branch. For the namespace to be put in '_nssChanges' we need at least two
                // entries.
                invariant(range.size() > 1);
                nextLowestCleanupTimestamp =
                    std::min(nextLowestCleanupTimestamp, cleanupTime(range));
                continue;
            }

            // The iterator is positioned to the closest entry that has a larger timestamp,
            // decrement to get a lower or equal timestamp. This represents the first entry that we
            // may not cleanup.
            --rangeIt;

            // Erase range, we will leave at least one element due to the decrement above
            range.erase(range.begin(), rangeIt);

            // If more changes are needed for this namespace, keep it in the set and keep track
            // of lowest timestamp.
            if (range.size() > 1) {
                nextLowestCleanupTimestamp =
                    std::min(nextLowestCleanupTimestamp, cleanupTime(range));
                ids.set(key, std::move(range));
                continue;
            }
            // If the last remaining element is a drop earlier than the oldest timestamp, we can
            // remove tracking this namespace
            if (range.back().id == boost::none) {
                ids.erase(key);
            } else {
                ids.set(key, std::move(range));
            }

            // Unmark this namespace or UUID for needing changes.
            changes.erase(key);
        }

        // Write back all changes to main container
        changesContainer = changes.persistent();
        idsContainer = ids.persistent();
    };

    // Iterate over all namespaces and UUIDs that is marked that they need cleanup
    doCleanup(_nss, _nssChanges);
    doCleanup(_uuid, _uuidChanges);

    _lowestTimestampForCleanup = nextLowestCleanupTimestamp;
    _oldestTimestampMaintained = std::max(_oldestTimestampMaintained, oldest);
}

void HistoricalCatalogIdTracker::rollback(Timestamp stable) {
    _nssChanges = {};
    _uuidChanges = {};
    _lowestTimestampForCleanup = Timestamp::max();
    _oldestTimestampMaintained = std::min(_oldestTimestampMaintained, stable);

    // Helper lambda to perform the operation on both namespace and UUID
    auto removeLargerTimestamps = [this, &stable](auto& idsContainer, auto& changesContainer) {
        // Batch all changes together
        auto idsWriter = idsContainer.transient();
        auto changesWriter = changesContainer.transient();

        // Go through all known mappings and remove entries larger than input stable timestamp
        for (const auto& [key, ids] : idsContainer) {
            // Binary search to the first entry with a too large timestamp
            auto end = std::upper_bound(
                ids.begin(), ids.end(), stable, [](Timestamp ts, const auto& entry) {
                    return ts < entry.ts;
                });

            // Create a new range without the timestamps that are too large
            std::vector<TimestampedCatalogId> removed(ids.begin(), end);

            // If the resulting range is empty, remove the key from the container
            if (removed.empty()) {
                idsWriter.erase(key);
                continue;
            }

            // Calculate when this namespace needs to be cleaned up next
            if (needsCleanup(removed)) {
                Timestamp cleanTime = cleanupTime(removed);
                changesWriter.insert(key);
                _recordCleanupTime(cleanTime);
            }
            idsWriter.set(key, std::move(removed));
        }

        // Write back all changes to main container
        changesContainer = changesWriter.persistent();
        idsContainer = idsWriter.persistent();
    };

    // Rollback on both namespace and uuid containers.
    removeLargerTimestamps(_nss, _nssChanges);
    removeLargerTimestamps(_uuid, _uuidChanges);
}

void HistoricalCatalogIdTracker::_recordCleanupTime(Timestamp ts) {
    if (ts < _lowestTimestampForCleanup) {
        _lowestTimestampForCleanup = ts;
    }
}

void HistoricalCatalogIdTracker::_createTimestamp(const NamespaceString& nss,
                                                  const UUID& uuid,
                                                  RecordId catalogId,
                                                  Timestamp ts) {
    // Helper lambda to perform the operation on both namespace and UUID
    auto doCreate = [&catalogId, &ts](auto& idsContainer, const auto& key) {
        // Make a copy of the vector stored at 'key'
        auto ids = copyIfExists(idsContainer, key);

        // An entry could exist already if concurrent writes are performed, keep the latest
        // change in that case.
        if (!ids.empty() && ids.back().ts == ts) {
            ids.back().id = catalogId;
            idsContainer = idsContainer.set(key, std::move(ids));
            return;
        }

        // Otherwise, push new entry at the end. Timestamp is always increasing
        invariant(ids.empty() || ids.back().ts < ts);
        // If the catalogId is the same as last entry, there's nothing we need to do. This can
        // happen when the catalog is reopened.
        if (!ids.empty() && ids.back().id == catalogId) {
            return;
        }

        // Push new mapping to the end and write back to the container. As this is a create, we do
        // not need to update the cleanup time as a create can never yield an updated (lower)
        // cleanup time for this namespace/uuid.
        ids.push_back({catalogId, ts});
        idsContainer = idsContainer.set(key, std::move(ids));
    };

    // Create on both namespace and uuid containers.
    doCreate(_nss, nss);
    doCreate(_uuid, uuid);
}

void HistoricalCatalogIdTracker::_createNoTimestamp(const NamespaceString& nss,
                                                    const UUID& uuid,
                                                    RecordId catalogId) {
    // Make sure untimestamped writes have a single entry in mapping. If we're mixing
    // timestamped with untimestamped (such as repair). Ignore the untimestamped writes
    // as an untimestamped deregister will correspond with an untimestamped register. We
    // should leave the mapping as-is in this case.

    auto doCreate = [&catalogId](auto& idsContainer, const auto& key) {
        const std::vector<TimestampedCatalogId>* ids = idsContainer.find(key);
        if (!ids) {
            // This namespace or UUID was added due to an untimestamped write, add an entry
            // with min timestamp
            idsContainer = idsContainer.set(key, {{catalogId, Timestamp::min()}});
            return;
        }

        if (ids->size() > 1 && !storageGlobalParams.repair) {
            // This namespace or UUID was added due to an untimestamped write. But this
            // namespace or UUID already had some timestamped writes performed. In this
            // case, we re-write the history. The only known area that does this today is
            // when profiling is enabled (untimestamped collection creation), followed by
            // dropping the database (timestamped collection drop).
            // TODO SERVER-75740: Remove this branch.
            invariant(!ids->back().ts.isNull());

            idsContainer = idsContainer.set(key, {{catalogId, Timestamp::min()}});
        }
    };

    // Create on both namespace and uuid containers.
    doCreate(_nss, nss);
    doCreate(_uuid, uuid);
}

void HistoricalCatalogIdTracker::_dropTimestamp(const NamespaceString& nss,
                                                const UUID& uuid,
                                                Timestamp ts) {
    // Helper lambda to perform the operation on both namespace and UUID
    auto doDrop = [this, &ts](auto& idsContainer, auto& changesContainer, const auto& key) {
        // Make a copy of the vector stored at 'key'
        auto ids = copyIfExists(idsContainer, key);
        // An entry could exist already if concurrent writes are performed, keep the latest change
        // in that case.
        if (!ids.empty() && ids.back().ts == ts) {
            ids.back().id = boost::none;
            idsContainer = idsContainer.set(key, std::move(ids));
            return;
        }

        // Otherwise, push new entry at the end. Timestamp is always increasing
        invariant(ids.empty() || ids.back().ts < ts);
        // If the catalogId is the same as last entry, there's nothing we need to do. This can
        // happen when the catalog is reopened.
        if (!ids.empty() && !ids.back().id.has_value()) {
            return;
        }

        // A drop entry can't be pushed in the container if it's empty. This is because we cannot
        // initialize the namespace or UUID with a single drop.
        invariant(!ids.empty());

        // Push the drop at the end our or mapping
        ids.push_back({boost::none, ts});

        // This drop may result in the possibility of cleanup in the future
        if (needsCleanup(ids)) {
            Timestamp cleanTime = cleanupTime(ids);
            changesContainer = changesContainer.insert(key);
            _recordCleanupTime(cleanTime);
        }

        // Write back the updated mapping into our container
        idsContainer = idsContainer.set(key, std::move(ids));
    };

    // Drop on both namespace and uuid containers
    doDrop(_nss, _nssChanges, nss);
    doDrop(_uuid, _uuidChanges, uuid);
}

void HistoricalCatalogIdTracker::_dropNoTimestamp(const NamespaceString& nss, const UUID& uuid) {
    // Make sure untimestamped writes have a single entry in mapping. If we're mixing
    // timestamped with untimestamped (such as repair). Ignore the untimestamped writes as
    // an untimestamped deregister will correspond with an untimestamped register. We should
    // leave the mapping as-is in this case.

    auto doDrop = [](auto& idsContainer, const auto& key) {
        const std::vector<TimestampedCatalogId>* ids = idsContainer.find(key);
        if (ids && ids->size() == 1) {
            // This namespace or UUID was removed due to an untimestamped write, clear entries.
            idsContainer = idsContainer.erase(key);
        }
    };

    // Drop on both namespace and uuid containers
    doDrop(_nss, nss);
    doDrop(_uuid, uuid);
}

void HistoricalCatalogIdTracker::_renameTimestamp(const NamespaceString& from,
                                                  const NamespaceString& to,
                                                  Timestamp ts) {
    // Make copies of existing mappings on these namespaces.
    auto toIds = copyIfExists(_nss, to);
    auto fromIds = copyIfExists(_nss, from);

    // First update 'to' mapping. This is similar to a 'create'.
    if (!toIds.empty() && toIds.back().ts == ts) {
        // An entry could exist already if concurrent writes are performed, keep the latest change
        // in that case.
        toIds.back().id = fromIds.back().id;
    } else {
        // Timestamps should always be increasing.
        invariant(toIds.empty() || toIds.back().ts < ts);

        // Push to end, we can take the catalogId from 'from'. We don't need to check if timestamp
        // needs to be cleaned up as this is equivalent of a 'create'.
        toIds.push_back({fromIds.back().id, ts});
    }

    // Then, update 'from' mapping. This is similar to a 'drop'.
    if (!fromIds.empty() && fromIds.back().ts == ts) {
        // Re-write latest entry if timestamp match (multiple changes occured in this transaction),
        // otherwise push at end.
        fromIds.back().id = boost::none;
    } else {
        // Timestamps should always be increasing.
        invariant(fromIds.empty() || fromIds.back().ts < ts);
        // Push to end and calculate cleanup timestamp.
        fromIds.push_back({boost::none, ts});
        if (needsCleanup(fromIds)) {
            Timestamp cleanTime = cleanupTime(fromIds);
            _nssChanges = std::move(_nssChanges).insert(from);
            _recordCleanupTime(cleanTime);
        }
    }

    // Store updates mappings back into container.
    auto writer = _nss.transient();
    writer.set(from, std::move(fromIds));
    writer.set(to, std::move(toIds));
    _nss = writer.persistent();
}

void HistoricalCatalogIdTracker::_renameNoTimestamp(const NamespaceString& from,
                                                    const NamespaceString& to) {
    // We should never perform rename in a mixed-mode environment. 'from' should contain a
    // single entry and there should be nothing in 'to' .
    const std::vector<TimestampedCatalogId>* fromIds = _nss.find(from);
    invariant(fromIds && fromIds->size() == 1);
    invariant(!_nss.find(to));

    auto writer = _nss.transient();
    // Take the last known catalogId from 'from'.
    writer.set(to, {{fromIds->back().id, Timestamp::min()}});
    writer.erase(from);
    _nss = writer.persistent();
}

}  // namespace mongo
