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
#include "mongo/db/service_context.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/string_map.h"

#include <set>
#include <string>

namespace mongo {
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
    bool collectionNeedsSizeAdjustment(StringData ident) const;

    /**
     * Returns whether 'ident' has been specifically marked as requiring adjustment even during
     * recovery.
     */
    bool collectionAlwaysNeedsSizeAdjustment(StringData ident) const;

    /**
     * Mark 'ident' as always requiring size adjustment, even if replication recovery is ongoing.
     */
    void markCollectionAsAlwaysNeedsSizeAdjustment(StringData ident);

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
    mutable stdx::mutex _mutex;
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
