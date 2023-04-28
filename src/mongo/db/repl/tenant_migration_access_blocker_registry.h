/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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
#include "mongo/db/repl/tenant_migration_donor_access_blocker.h"
#include "mongo/db/repl/tenant_migration_recipient_access_blocker.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/string_map.h"

namespace mongo {

class TenantMigrationAccessBlockerRegistry {
    TenantMigrationAccessBlockerRegistry(const TenantMigrationAccessBlockerRegistry&) = delete;
    TenantMigrationAccessBlockerRegistry& operator=(const TenantMigrationAccessBlockerRegistry&) =
        delete;

public:
    struct DonorRecipientAccessBlockerPair {
        DonorRecipientAccessBlockerPair() = default;
        DonorRecipientAccessBlockerPair(
            std::shared_ptr<TenantMigrationDonorAccessBlocker> donor,
            std::shared_ptr<TenantMigrationRecipientAccessBlocker> recipient)
            : _donor(std::move(donor)), _recipient(std::move(recipient)) {}
        DonorRecipientAccessBlockerPair(std::shared_ptr<TenantMigrationDonorAccessBlocker> donor)
            : _donor(std::move(donor)) {}
        DonorRecipientAccessBlockerPair(
            std::shared_ptr<TenantMigrationRecipientAccessBlocker> recipient)
            : _recipient(std::move(recipient)) {}

        std::shared_ptr<TenantMigrationAccessBlocker> getAccessBlocker(
            TenantMigrationAccessBlocker::BlockerType type) const {
            if (type == TenantMigrationAccessBlocker::BlockerType::kDonor) {
                return _donor;
            }
            return _recipient;
        }

        std::shared_ptr<TenantMigrationDonorAccessBlocker> getDonorAccessBlocker() const {
            return _donor;
        }

        std::shared_ptr<TenantMigrationRecipientAccessBlocker> getRecipientAccessBlocker() const {
            return _recipient;
        }

        void setAccessBlocker(std::shared_ptr<TenantMigrationAccessBlocker> mtab) {
            invariant(mtab);
            if (mtab->getType() == TenantMigrationAccessBlocker::BlockerType::kDonor) {
                invariant(!_donor);
                _donor = checked_pointer_cast<TenantMigrationDonorAccessBlocker>(std::move(mtab));
            } else {
                invariant(!_recipient);
                _recipient =
                    checked_pointer_cast<TenantMigrationRecipientAccessBlocker>(std::move(mtab));
            }
        }

        void clearAccessBlocker(TenantMigrationAccessBlocker::BlockerType type) {
            if (type == TenantMigrationAccessBlocker::BlockerType::kDonor) {
                if (!_donor) {
                    return;
                }
                _donor->interrupt();
                _donor.reset();
            } else {
                if (!_recipient) {
                    return;
                }
                _recipient.reset();
            }
        }

    private:
        std::shared_ptr<TenantMigrationDonorAccessBlocker> _donor;
        std::shared_ptr<TenantMigrationRecipientAccessBlocker> _recipient;
    };

    TenantMigrationAccessBlockerRegistry();

    static const ServiceContext::Decoration<TenantMigrationAccessBlockerRegistry> get;

    /**
     * Adds an entry for mtab with the provided tenantId as the key. There must not be
     * a blocker of the same type (donor or recipient) for this tenantId already.
     */
    void add(const TenantId& tenantId, std::shared_ptr<TenantMigrationAccessBlocker> mtab);

    /**
     * Add one access blocker and associate it with many tenant_id objects.
     */
    void add(const std::vector<TenantId>& tenantIds,
             std::shared_ptr<TenantMigrationAccessBlocker> mtab);

    /**
     * Adds an entry for mtab that will block all tenants.
     */
    void addGlobalDonorAccessBlocker(std::shared_ptr<TenantMigrationDonorAccessBlocker> mtab);

    /**
     * Remove all access blockers of the provided type for a migration.
     */
    void removeAccessBlockersForMigration(const UUID& migrationId,
                                          TenantMigrationAccessBlocker::BlockerType type);

    /**
     * Removes all mtabs of the given type.
     */
    void removeAll(TenantMigrationAccessBlocker::BlockerType type);

    /**
     * Returns the first 'DonorRecipientAccessBlockerPair' it finds whose 'tenantId' is a prefix for
     * 'dbName'.
     */
    boost::optional<DonorRecipientAccessBlockerPair> getAccessBlockersForDbName(
        const DatabaseName& dbName);

    /**
     * Iterates through each of the TenantMigrationAccessBlockers and
     * returns the first 'TenantMigrationAccessBlocker' it finds whose 'tenantId' is a prefix for
     * 'dbName' and is of the requested type.
     */
    std::shared_ptr<TenantMigrationAccessBlocker> getTenantMigrationAccessBlockerForDbName(
        const DatabaseName& dbName, TenantMigrationAccessBlocker::BlockerType type);

    /**
     * Return the global donor access blocker or searches through TenantMigrationAccessBlockers and
     * returns the TenantMigrationAccessBlocker that matches tenantId.
     */
    std::shared_ptr<TenantMigrationAccessBlocker> getTenantMigrationAccessBlockerForTenantId(
        const TenantId& tenantId, TenantMigrationAccessBlocker::BlockerType type);

    /**
     * Return the global donor access blocker and all the donor access blockers associated with a
     * migration.
     */
    std::vector<std::shared_ptr<TenantMigrationDonorAccessBlocker>>
    getDonorAccessBlockersForMigration(const UUID& migrationId);

    /**
     * Return the recipient access blockers associated with a migration.
     */
    std::vector<std::shared_ptr<TenantMigrationRecipientAccessBlocker>>
    getRecipientAccessBlockersForMigration(const UUID& migrationId);

    using applyAllCallback = std::function<void(
        const TenantId& tenantId, std::shared_ptr<TenantMigrationAccessBlocker>& mtab)>;
    /**
     * Applies callback to all TenantMigrationAccessBlockers of the desired type.
     */
    void applyAll(TenantMigrationAccessBlocker::BlockerType type, applyAllCallback&& callback);

    /**
     * Starts the _asyncBlockingOperationsExecutor.
     */
    void startup();

    /**
     * Shuts down each of the TenantMigrationAccessBlockers and releases the shared_ptrs to the
     * TenantMigrationAccessBlockers from the map.
     */
    void clear();

    /**
     * Shuts down each of the TenantMigrationAccessBlockers, releases the shared_ptrs to the
     * TenantMigrationAccessBlockers from the map, and resets the executor.
     */
    void shutDown();

    /**
     * Iterates through each of the TenantMigrationAccessBlockers stored by the mapping
     * and appends the server status of each blocker to the BSONObjBuilder.
     */
    void appendInfoForServerStatus(BSONObjBuilder* builder) const;

    /**
     * Notifies all the TenantMigrationAccessBlockers that the given opTime has been majority
     * committed.
     *
     * This is called while holding a very hot mutex (the ReplicationCoordinator mutex). Therefore
     * it should avoid doing any work that can be done later, and avoid calling back into any
     * replication functions that take this mutex (which would cause self-deadlock).
     */
    void onMajorityCommitPointUpdate(repl::OpTime opTime);

    std::shared_ptr<executor::TaskExecutor> getAsyncBlockingOperationsExecutor() const;

private:
    void _remove(WithLock,
                 const TenantId& tenantId,
                 TenantMigrationAccessBlocker::BlockerType type);

    void _clear(WithLock);

    std::shared_ptr<TenantMigrationDonorAccessBlocker> _getGlobalTenantDonorAccessBlocker(
        WithLock) const;

    std::shared_ptr<TenantMigrationDonorAccessBlocker> _getGlobalTenantDonorAccessBlocker(
        WithLock, const DatabaseName& dbName) const;

    mutable Mutex _mutex = MONGO_MAKE_LATCH("TenantMigrationAccessBlockerRegistry::_mutex");

    // All member variables are labeled with one of the following codes indicating the
    // synchronization rules for accessing them.
    //
    // (R)  Read-only in concurrent operation; no synchronization required.
    // (S)  Self-synchronizing; access according to class's own rules.
    // (M)  Reads and writes guarded by _mutex.
    // (W)  Synchronization required only for writes.
    std::shared_ptr<executor::TaskExecutor>
        _asyncBlockingOperationsExecutor;  // (S) Lives for the lifetime of the registry.

    stdx::unordered_map<TenantId, DonorRecipientAccessBlockerPair, TenantId::Hasher>
        _tenantMigrationAccessBlockers;  // (M)
};

}  // namespace mongo
