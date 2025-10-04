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

#include "mongo/base/string_data.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/ident.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/string_map.h"

#include <cstddef>
#include <memory>
#include <set>
#include <string>

#include <boost/optional.hpp>

namespace mongo {

class Ident;

/**
 * This class manages idents in the KV storage engine that are marked as drop-pending by the
 * two-phase index/collection drop algorithm.
 *
 * Replicated collections and indexes that are dropped are not permanently removed from the storage
 * system when the drop request is first processed. The catalog entry is removed to render the
 * collection or index inaccessible to user operations but the underlying ident is retained in the
 * event we have to recover the collection. This is relevant mostly to rollback and startup
 * recovery.
 *
 * On receiving a notification that the oldest timestamp queued has advanced and become part of the
 * checkpoint, some drop-pending idents will become safe to remove permanently. The function
 * dropldentsOlderThan() is provided for this purpose.
 *
 * Additionally, the reaper will not drop an ident (record store) while any other references to the
 * Ident passed in via addDropPendingIdent remain. This ensures that there are no remaining
 * concurrent operations still using the record store, which is essential because a data drop is
 * unversioned: once it is dropped, it is gone for all readers.
 */
class KVDropPendingIdentReaper {
public:
    explicit KVDropPendingIdentReaper(KVEngine* engine);

    /**
     * Adds a new drop-pending ident, with its drop time and namespace, to be managed by this
     * class.
     *
     * When the drop time is old enough and no remaining operations reference the 'ident', then the
     * index/collection data will be safe to drop unversioned.
     *
     * A drop time is considered old enough when:
     * - (Timestamp) The op cannot be rolled back nor new users access the record store data.
     * - (CheckpointIteration) The catalog has made its changes durable.
     *
     * onDrop must not call dropIdentsOlderThan() or immediatelyCompletePendingDrop().
     */
    void addDropPendingIdent(const StorageEngine::DropTime& dropTime,
                             std::shared_ptr<Ident> ident,
                             StorageEngine::DropIdentCallback&& onDrop = nullptr);

    /**
     * Adds an ident with an unknown drop time that is no later than `stableTimestamp`.
     *
     * When opening the catalog, all idents found in the storage engine but not the catalog are
     * dropped. The exact drop time is unknown, but as it is known to be missing at the stable
     * timestamp it must either be no greater than that or be an ident *created* after the stable
     * timestamp.
     */
    void dropUnknownIdent(const Timestamp& stableTimestamp, StringData ident);

    /**
     * Marks the ident as in use and prevents the reaper from dropping the ident.
     *
     * Returns nullptr if the ident is not found, or if the ident state is `kBeingDropped`. Returns
     * a shared_ptr to the `dropToken` if it isn't expired, otherwise a new shared_ptr is generated,
     * stored in `dropToken`, and returned.
     */
    std::shared_ptr<Ident> markIdentInUse(StringData ident);

    /**
     * Returns earliest drop timestamp in '_dropPendingIdents'.
     * Returns boost::none if '_dropPendingIdents' is empty.
     */
    boost::optional<Timestamp> getEarliestDropTimestamp() const;

    // Returns whether there are expired idents
    bool hasExpiredIdents(const Timestamp& ts) const;

    /**
     * Returns a list of ident names currently tracked by the reaper.
     */
    std::vector<std::string> getAllIdentNames() const;

    /**
     * Returns the number of drop-pending idents.
     */
    size_t getNumIdents() const;

    /**
     * Notifies this class that the storage engine has advanced its oldest timestamp.
     * Drops all unreferenced drop-pending idents with drop timestamps before 'ts', as well as all
     * unreferenced idents with Timestamp::min() drop timestamps (untimestamped on standalones) as
     * long as the changes have been checkpointed.
     */
    void dropIdentsOlderThan(OperationContext* opCtx, const Timestamp& ts);

    /**
     * Clears maps of drop pending idents for drops with timestamps greater than or equal to the
     * given stable timestamp.
     *
     * This function is called following a rollback-to-stable to roll back all drops which occurred
     * after the stable timestamp. It must be called before the timestamp listener is restarted
     * following RTS. Idents which were created and then dropped after the stable timestamp will
     * have already been converted to untimestamped drops as part of opening the catalog at the
     * stable timestamp.
     */
    void rollbackDropsAfterStableTimestamp(Timestamp stableTimestamp);

    /**
     * If the given ident has been registered with the reaper, attempts to immediately drop it,
     * possibly blocking while the background thread is reaping idents. Returns ObjectIsBusy if the
     * ident could not be dropped due to being in use. Returns Status::OK() if the ident was not
     * tracked as the reaper cannot distinguish "ident has already been dropped" from "ident was
     * never drop pending".
     */
    Status immediatelyCompletePendingDrop(OperationContext* opCtx, StringData ident);

private:
    // Contains information identifying what collection/index data to drop as well as determining
    // when to do so.
    struct IdentInfo {
        // Identifier for the storage to drop the associated collection or index data.
        std::string identName;

        // The collection or index data can be safely dropped when no references to this token
        // remain and the catalog has checkpointed the changes. The latter is mostly useful for
        // untimestamped writes.
        StorageEngine::DropTime dropTime;
        std::weak_ptr<Ident> dropToken;

        // Callback to run once the ident has been dropped.
        StorageEngine::DropIdentCallback onDrop;

        // Set to false if the dropTime is an upper bound rather than the exact drop time
        bool dropTimeIsExact = true;
        // Set to true while the ident is in the process of being dropped. Idents are not
        // unregistered until the drop has completed, but once a drop has started it's too late to
        // keep the ident alive.
        bool dropInProgress = false;

        bool isExpired(const KVEngine* engine, const Timestamp& ts) const;
    };

    Status _tryToDrop(WithLock,  // Must hold _dropMutex but *not* _mutex
                      OperationContext* opCtx,
                      IdentInfo& identInfo);

    Status _immediatelyAttemptToCompletePendingDrop(OperationContext* opCtx, StringData ident);

    struct CompareByDropTime {
        using is_transparent = bool;
        bool operator()(const std::shared_ptr<IdentInfo>& a,
                        const std::shared_ptr<IdentInfo>& b) const;
        bool operator()(const StorageEngine::DropTime& a,
                        const std::shared_ptr<IdentInfo>& b) const;
        bool operator()(const std::shared_ptr<IdentInfo>& a,
                        const StorageEngine::DropTime& b) const;
    };

    // Container type for drop-pending namespaces. We use a multiset so that we can order the
    // namespaces by drop optime. Additionally, it is possible for certain user operations (such
    // as renameCollection across databases) to generate more than one drop-pending namespace for
    // the same drop optime.
    using DropPendingIdents = std::multiset<std::shared_ptr<IdentInfo>, CompareByDropTime>;

    // Used to access the KV engine for the purposes of dropping the ident.
    KVEngine* const _engine;

    /**
     * Mutex which is held while performing drop operations to ensure that concurrent calls to
     * immediatelyCompletePendingDrop() or dropIdentsOlderThan() are serialized. Must be acquired
     * *before* `_mutex`. The full flow for dropping is:
     *
     * 1. Acquire _dropMutex
     * 2. Acquire _mutex
     * 3. Find ident(s) to drop.
     * 4. Set dropInProgress = true
     * 5. Release _mutex
     * 6. Attempt to drop ident
     * 7. Acquire _mutex
     * 8. Either set dropInProgress = false or remove ident from maps
     * 9. Release _mutex
     * 10. Release _dropMutex
     *
     * Note that dropInProgress should only ever be true while a thread holds
     * _dropMutex. This is used so that markIdentInUse() can avoid returning an ident which is being
     * dropped without having to acquire _dropMutex.
     */
    mutable stdx::mutex _dropMutex;

    // Guards access to member variables below.
    mutable stdx::mutex _mutex;

    // Drop-pending idents. Ordered by drop timestamp.
    DropPendingIdents _timestampOrderedIdents;

    // Ident to drop timestamp map. Used for efficient lookups into _timestampOrderedIdents.
    StringMap<std::shared_ptr<IdentInfo>> _dropPendingIdents;
};

}  // namespace mongo
