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


#include "mongo/db/repl/tenant_migration_access_blocker_registry.h"
#include "mongo/db/repl/tenant_migration_access_blocker.h"
#include "mongo/db/repl/tenant_migration_access_blocker_util.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/util/concurrency/thread_pool.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTenantMigration

namespace mongo {

namespace {

constexpr char kBlockAllTenantsKey[] = "__ALL__";

// Executor to asynchronously schedule blocking operations while the tenant migration access
// blockers are in action. This provides migrated tenants isolation from the non-migrated users.
// The executor is shared by all access blockers and the thread count goes to 0 when there is no
// migration.
std::shared_ptr<executor::TaskExecutor> _createBlockedOperationsExecutor() {
    ThreadPool::Options threadPoolOptions;
    threadPoolOptions.maxThreads = 4;
    // When there is no migration, reduce thread count to 0.
    threadPoolOptions.minThreads = 0;
    threadPoolOptions.threadNamePrefix = "TenantMigrationBlockerAsync-";
    threadPoolOptions.poolName = "TenantMigrationBlockerAsyncThreadPool";
    threadPoolOptions.onCreateThread = [](const std::string& threadName) {
        Client::initThread(threadName.c_str());
    };
    auto executor = std::make_shared<executor::ThreadPoolTaskExecutor>(
        std::make_unique<ThreadPool>(threadPoolOptions),
        executor::makeNetworkInterface("TenantMigrationBlockerNet"));

    return executor;
}
}  // namespace

using MtabType = TenantMigrationAccessBlocker::BlockerType;
using MtabPair = TenantMigrationAccessBlockerRegistry::DonorRecipientAccessBlockerPair;

const ServiceContext::Decoration<TenantMigrationAccessBlockerRegistry>
    TenantMigrationAccessBlockerRegistry::get =
        ServiceContext::declareDecoration<TenantMigrationAccessBlockerRegistry>();

void TenantMigrationAccessBlockerRegistry::add(StringData tenantId,
                                               std::shared_ptr<TenantMigrationAccessBlocker> mtab) {
    invariant(!tenantId.empty());

    stdx::lock_guard<Latch> lg(_mutex);
    auto mtabType = mtab->getType();
    tassert(8423350,
            "Trying to add a multi-tenant migration donor blocker when this node already has a "
            "donor blocker for all tenants",
            mtabType != MtabType::kDonor || !_getAllTenantDonorAccessBlocker(lg));

    const auto& it = _tenantMigrationAccessBlockers.find(tenantId);
    if (it != _tenantMigrationAccessBlockers.end()) {
        auto existingMtab = it->second.getAccessBlocker(mtabType);
        if (existingMtab) {
            uasserted(ErrorCodes::ConflictingServerlessOperation,
                      str::stream() << "This node is already a "
                                    << (mtabType == MtabType::kDonor ? "donor" : "recipient")
                                    << " for tenantId \"" << tenantId << "\" with migrationId \""
                                    << existingMtab->getMigrationId().toString() << "\"");
        }
        // The migration protocol guarantees that the original donor node must be garbage collected
        // before it can be chosen as a recipient under the same tenant. Therefore, we only expect
        // to have both recipient and donor access blockers in the case of back-to-back migrations
        // where the node participates first as a recipient then a donor.
        invariant(mtabType == MtabType::kDonor);
        it->second.setAccessBlocker(std::move(mtab));
        return;
    }
    MtabPair mtabPair;
    mtabPair.setAccessBlocker(std::move(mtab));
    _tenantMigrationAccessBlockers.emplace(tenantId, mtabPair);
}

void TenantMigrationAccessBlockerRegistry::add(const std::vector<StringData>& tenantIds,
                                               std::shared_ptr<TenantMigrationAccessBlocker> mtab) {
    for (auto&& tenantId : tenantIds) {
        add(tenantId, mtab);
    }
}

void TenantMigrationAccessBlockerRegistry::add(std::shared_ptr<TenantMigrationAccessBlocker> mtab) {
    LOGV2_DEBUG(6114102, 1, "Adding donor access blocker for all tenants");
    stdx::lock_guard<Latch> lg(_mutex);

    const auto donorAccessBlocker = _getAllTenantDonorAccessBlocker(lg);
    tassert(ErrorCodes::ConflictingOperationInProgress,
            str::stream() << "This node is already a donor with migrationId "
                          << donorAccessBlocker->getMigrationId().toString(),
            !donorAccessBlocker);

    const auto& foundAccessBlocker =
        std::find_if(_tenantMigrationAccessBlockers.begin(),
                     _tenantMigrationAccessBlockers.end(),
                     [](const auto& pair) { return pair.second.getDonorAccessBlocker().get(); });
    uassert(ErrorCodes::ConflictingServerlessOperation,
            str::stream()
                << "Trying to add donor blocker for all tenants when this node already has a donor "
                   "blocker for \""
                << foundAccessBlocker->second.getDonorAccessBlocker()->getMigrationId().toString()
                << "\"",
            foundAccessBlocker == _tenantMigrationAccessBlockers.end());

    MtabPair mtabPair;
    mtabPair.setAccessBlocker(std::move(mtab));
    _tenantMigrationAccessBlockers.emplace(kBlockAllTenantsKey, mtabPair);
}

void TenantMigrationAccessBlockerRegistry::_remove(WithLock, StringData tenantId, MtabType type) {
    const auto& it = _tenantMigrationAccessBlockers.find(tenantId);

    if (it == _tenantMigrationAccessBlockers.end()) {
        return;
    }

    // Use a reference to the DonorRecipientAccessBlockerPair so that we manipulate the actual
    // value as opposed to a copy.
    auto& mtabPair = it->second;
    mtabPair.clearAccessBlocker(type);
    if (!mtabPair.getDonorAccessBlocker() && !mtabPair.getRecipientAccessBlocker()) {
        _tenantMigrationAccessBlockers.erase(it);
    }
}

void TenantMigrationAccessBlockerRegistry::remove(StringData tenantId, MtabType type) {
    stdx::lock_guard<Latch> lg(_mutex);
    if (type == MtabType::kDonor && _getAllTenantDonorAccessBlocker(lg)) {
        tasserted(8423348, "Using remove() for new-style donor access blocker");
    }

    _remove(lg, tenantId, type);
}

void TenantMigrationAccessBlockerRegistry::removeAccessBlockersForMigration(
    const UUID& migrationId, TenantMigrationAccessBlocker::BlockerType type) {
    stdx::lock_guard<Latch> lg(_mutex);
    if (const auto donorAccessBlocker = _getAllTenantDonorAccessBlocker(lg);
        type == MtabType::kDonor && donorAccessBlocker) {
        if (donorAccessBlocker->getMigrationId() != migrationId) {
            return;
        }
    }

    // Clear blockers for migrationId, and erase pairs with no blocker remaining.
    erase_if(_tenantMigrationAccessBlockers,
             [&](std::pair<const std::string, DonorRecipientAccessBlockerPair> it) {
                 auto& mtabPair = it.second;
                 auto blocker = mtabPair.getAccessBlocker(type);
                 if (!blocker || blocker->getMigrationId() != migrationId) {
                     return false;
                 }

                 mtabPair.clearAccessBlocker(type);
                 MtabType oppositeType;
                 switch (type) {
                     case MtabType::kRecipient:
                         oppositeType = MtabType::kDonor;
                         break;
                     case MtabType::kDonor:
                         oppositeType = MtabType::kRecipient;
                         break;
                     default:
                         MONGO_UNREACHABLE;
                 }
                 return !mtabPair.getAccessBlocker(oppositeType);
             });
}

void TenantMigrationAccessBlockerRegistry::removeAll(MtabType type) {
    stdx::lock_guard<Latch> lg(_mutex);
    if (auto donorAccessBlocker = _getAllTenantDonorAccessBlocker(lg); donorAccessBlocker) {
        donorAccessBlocker->interrupt();
    }

    for (auto it = _tenantMigrationAccessBlockers.begin();
         it != _tenantMigrationAccessBlockers.end();) {
        _remove(lg, (it++)->first, type);
    }
}

boost::optional<MtabPair> TenantMigrationAccessBlockerRegistry::getAccessBlockersForDbName(
    StringData dbName) {
    stdx::lock_guard<Latch> lg(_mutex);
    auto donorAccessBlocker = _getAllTenantDonorAccessBlocker(lg, dbName);
    auto tenantId = tenant_migration_access_blocker::parseTenantIdFromDB(dbName);

    if (!tenantId && donorAccessBlocker) {
        return MtabPair{donorAccessBlocker};
    }

    if (!tenantId) {
        return boost::none;
    }

    const auto& it = _tenantMigrationAccessBlockers.find(tenantId.get());

    if (it != _tenantMigrationAccessBlockers.end() && donorAccessBlocker) {
        return MtabPair{donorAccessBlocker, it->second.getRecipientAccessBlocker()};
    }

    if (donorAccessBlocker) {
        return MtabPair{donorAccessBlocker};
    }

    if (it != _tenantMigrationAccessBlockers.end()) {
        // Return a copy of the DonorRecipientAccessBlockerPair to the caller so that it
        // can be inspected and/or manipulated without changing the value in the registry.
        return it->second;
    }

    return boost::none;
}

std::shared_ptr<TenantMigrationAccessBlocker>
TenantMigrationAccessBlockerRegistry::getTenantMigrationAccessBlockerForDbName(StringData dbName,
                                                                               MtabType type) {
    auto mtabPair = getAccessBlockersForDbName(dbName);
    if (!mtabPair) {
        return nullptr;
    }

    return mtabPair->getAccessBlocker(type);
}

std::shared_ptr<TenantMigrationDonorAccessBlocker>
TenantMigrationAccessBlockerRegistry::_getAllTenantDonorAccessBlocker(WithLock) const {
    const auto& it = _tenantMigrationAccessBlockers.find(kBlockAllTenantsKey);
    if (it == _tenantMigrationAccessBlockers.end()) {
        return nullptr;
    }

    return checked_pointer_cast<TenantMigrationDonorAccessBlocker>(
        it->second.getDonorAccessBlocker());
}

std::shared_ptr<TenantMigrationAccessBlocker>
TenantMigrationAccessBlockerRegistry::getAccessBlockerForMigration(
    const UUID& migrationId, TenantMigrationAccessBlocker::BlockerType type) {
    stdx::lock_guard<Latch> lg(_mutex);
    auto mtab =
        std::find_if(_tenantMigrationAccessBlockers.begin(),
                     _tenantMigrationAccessBlockers.end(),
                     [type, migrationId](const auto& pair) {
                         return pair.second.getAccessBlocker(type)->getMigrationId() == migrationId;
                     });

    if (mtab == _tenantMigrationAccessBlockers.end()) {
        return nullptr;
    }

    return mtab->second.getAccessBlocker(type);
}

std::shared_ptr<TenantMigrationDonorAccessBlocker>
TenantMigrationAccessBlockerRegistry::_getAllTenantDonorAccessBlocker(WithLock lk,
                                                                      StringData dbName) const {
    // No-op oplog entries, e.g. for linearizable reads, use namespace "".
    bool isInternal = (dbName == "" || NamespaceString(dbName).isOnInternalDb());
    if (isInternal) {
        return nullptr;
    }

    return _getAllTenantDonorAccessBlocker(lk);
}

std::shared_ptr<TenantMigrationAccessBlocker>
TenantMigrationAccessBlockerRegistry::getTenantMigrationAccessBlockerForTenantId(
    StringData tenantId, MtabType type) {
    stdx::lock_guard<Latch> lg(_mutex);
    if (auto donorAccessBlocker = _getAllTenantDonorAccessBlocker(lg);
        type == MtabType::kDonor && donorAccessBlocker) {
        return donorAccessBlocker;
    }

    const auto& it = _tenantMigrationAccessBlockers.find(tenantId);
    if (it != _tenantMigrationAccessBlockers.end()) {
        return it->second.getAccessBlocker(type);
    }
    return nullptr;
}

void TenantMigrationAccessBlockerRegistry::applyAll(TenantMigrationAccessBlocker::BlockerType type,
                                                    applyAllCallback&& callback) {
    stdx::lock_guard<Latch> lg(_mutex);
    for (auto& [tenantId, mtabPair] : _tenantMigrationAccessBlockers) {
        if (auto mtab = mtabPair.getAccessBlocker(type)) {
            callback(tenantId, mtab);
        }
    }
}

void TenantMigrationAccessBlockerRegistry::shutDown() {
    stdx::lock_guard<Latch> lg(_mutex);
    _tenantMigrationAccessBlockers.clear();
    _asyncBlockingOperationsExecutor.reset();
}

void TenantMigrationAccessBlockerRegistry::appendInfoForServerStatus(
    BSONObjBuilder* builder) const {
    stdx::lock_guard<Latch> lg(_mutex);

    if (const auto donorAccessBlocker = _getAllTenantDonorAccessBlocker(lg); donorAccessBlocker) {
        BSONObjBuilder donorMtabInfoBuilder;
        donorAccessBlocker->appendInfoForServerStatus(&donorMtabInfoBuilder);
        builder->append("donor", donorMtabInfoBuilder.obj());
    }

    for (auto& [tenantId, mtabPair] : _tenantMigrationAccessBlockers) {
        BSONObjBuilder mtabInfoBuilder;

        if (auto donorMtab = mtabPair.getDonorAccessBlocker()) {
            BSONObjBuilder donorMtabInfoBuilder;
            donorMtab->appendInfoForServerStatus(&donorMtabInfoBuilder);
            mtabInfoBuilder.append("donor", donorMtabInfoBuilder.obj());
        }

        if (auto recipientMtab = mtabPair.getRecipientAccessBlocker()) {
            BSONObjBuilder recipientMtabInfoBuilder;
            recipientMtab->appendInfoForServerStatus(&recipientMtabInfoBuilder);
            mtabInfoBuilder.append("recipient", recipientMtabInfoBuilder.obj());
        }

        if (mtabInfoBuilder.len()) {
            builder->append(tenantId, mtabInfoBuilder.obj());
        }
    }
}

void TenantMigrationAccessBlockerRegistry::onMajorityCommitPointUpdate(repl::OpTime opTime) {
    stdx::lock_guard<Latch> lg(_mutex);

    for (auto& [_, mtabPair] : _tenantMigrationAccessBlockers) {
        if (auto recipientMtab = mtabPair.getRecipientAccessBlocker()) {
            recipientMtab->onMajorityCommitPointUpdate(opTime);
        }
        if (auto donorMtab = mtabPair.getDonorAccessBlocker()) {
            donorMtab->onMajorityCommitPointUpdate(opTime);
        }
    }

    if (auto donorAccessBlocker = _getAllTenantDonorAccessBlocker(lg); donorAccessBlocker) {
        donorAccessBlocker->onMajorityCommitPointUpdate(opTime);
    }
}

std::shared_ptr<executor::TaskExecutor>
TenantMigrationAccessBlockerRegistry::getAsyncBlockingOperationsExecutor() {
    stdx::lock_guard<Latch> lg(_mutex);
    if (!_asyncBlockingOperationsExecutor) {
        _asyncBlockingOperationsExecutor = _createBlockedOperationsExecutor();
        _asyncBlockingOperationsExecutor->startup();
    }

    return _asyncBlockingOperationsExecutor;
}

}  // namespace mongo
