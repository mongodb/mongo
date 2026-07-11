// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/service_context.h"
#include "mongo/util/modules.h"
#include "mongo/util/string_map.h"

#include <mutex>
#include <string_view>

namespace [[MONGO_MOD_PUBLIC]] mongo {
/**
 * This class is for use with storage engines that track record store sizes in catalog metadata.
 *
 * During normal server operation, we adjust the size metadata for all record stores. But when
 * performing replication recovery, we avoid doing so, as we trust that the size metadata on disk is
 * already correct with respect to the end state of recovery.
 *
 * However, there may be exceptions that require the server to adjust size metadata even during
 * recovery. One such case is the oplog: during rollback, the oplog is truncated, and then recovery
 * occurs using oplog entries after the common point from the sync source. The server will need to
 * adjust the size metadata for the oplog collection to ensure that the count of oplog entries is
 * correct after rollback recovery.
 *
 * This class is responsible for keeping track of idents that require this special
 * count adjustment.
 */
class SizeRecoveryState {
public:
    /**
     * If replication recovery is ongoing, returns false unless 'ident' has been specifically marked
     * as requiring adjustment even during recovery.
     *
     * If the system is not currently undergoing replication recovery, always returns true.
     */
    bool collectionNeedsSizeAdjustment(std::string_view ident) const;

    /**
     * Returns whether 'ident' has been specifically marked as requiring adjustment even during
     * recovery.
     */
    bool collectionAlwaysNeedsSizeAdjustment(std::string_view ident) const;

    /**
     * Mark 'ident' as always requiring size adjustment, even if replication recovery is ongoing.
     */
    void markCollectionAsAlwaysNeedsSizeAdjustment(std::string_view ident);

    /**
     * Clears all internal state. This method should be called before calling 'recover to a stable
     * timestamp'.
     */
    void clearStateBeforeRecovery();

    /**
     * Informs the SizeRecoveryState that record stores should always check their size information.
     */
    void setRecordStoresShouldAlwaysCheckSize(bool);

    /**
     * Returns whether record stores should always check their size information. This can either be
     * due to setRecordStoresShouldAlwaysCheckSize being called or due to being in replication
     * recovery.
     */
    bool shouldRecordStoresAlwaysCheckSize() const;

private:
    mutable std::mutex _mutex;
    StringSet _collectionsAlwaysNeedingSizeAdjustment;
    bool _recordStoresShouldAlwayCheckSize = false;
};

/**
 * Returns a mutable reference to the single SizeRecoveryState associated with 'serviceCtx'.
 */
SizeRecoveryState& sizeRecoveryState(ServiceContext* serviceCtx);

/**
 * The "in replication recovery" flag. Provided by a thread-safe decorator on ServiceContext, this
 * class provides an RAII utility around setting the flag value and allowing it to be checked by
 * readers. Note that this flag is advisory-only: writers will not check for readers and no further
 * synchronization is performed.
 */
class InReplicationRecovery final {
    InReplicationRecovery(const InReplicationRecovery&) = delete;
    InReplicationRecovery(InReplicationRecovery&&) = delete;
    InReplicationRecovery& operator=(const InReplicationRecovery&) = delete;
    InReplicationRecovery& operator=(InReplicationRecovery&&) = delete;

    ServiceContext* _serviceContext{nullptr};

public:
    /**
     * Constructing this class increments the `inReplicationRecovery` flag on
     * the provided ServiceContext. The flag will be decremented upon
     * destruction.
     */
    explicit InReplicationRecovery(ServiceContext*);
    ~InReplicationRecovery();

    /**
     * Checks whether the flag is non-zero, indicating at least 1 context has
     * declared that replication recovery is happening.
     */
    static bool isSet(ServiceContext*);
};
}  // namespace mongo
