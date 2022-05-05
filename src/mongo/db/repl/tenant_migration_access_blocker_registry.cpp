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
    stdx::lock_guard<Latch> lg(_mutex);
    auto mtabType = mtab->getType();
    tassert(8423351,
            "add called with new-style blocker, use addShardMergeDonorAccessBlocker instead",
            mtabType != MtabType::kDonor ||
                mtab->getProtocol() != MigrationProtocolEnum::kShardMerge);

    tassert(
        8423350,
        "Adding multitenant migration donor blocker when this node has a shard merge donor blocker",
        mtabType != MtabType::kDonor || !_donorAccessBlocker);

    auto it = _tenantMigrationAccessBlockers.find(tenantId);
    if (it != _tenantMigrationAccessBlockers.end()) {
        if (it->second.getAccessBlocker(mtabType)) {
            tasserted(ErrorCodes::ConflictingOperationInProgress,
                      str::stream() << "This node is already a "
                                    << MigrationProtocol_serializer(mtab->getProtocol()) << " "
                                    << (mtabType == MtabType::kDonor ? "donor" : "recipient")
                                    << " for tenantId \"" << tenantId << "\"");
        }
        // The migration protocol guarantees that the original donor node must be garbage collected
        // before it can be chosen as a recipient under the same tenant. Therefore, we only expect
        // to have both recipient and donor access blockers in the case of back-to-back migrations
        // where the node participates first as a recipient then a donor.
        invariant(mtabType == MtabType::kDonor);
        it->second.setAccessBlocker(mtab);
        return;
    }
    MtabPair mtabPair;
    mtabPair.setAccessBlocker(mtab);
    _tenantMigrationAccessBlockers.emplace(tenantId, mtabPair);
}

void TenantMigrationAccessBlockerRegistry::addShardMergeDonorAccessBlocker(
    std::shared_ptr<TenantMigrationDonorAccessBlocker> mtab) {
    LOGV2_DEBUG(6114102, 1, "Adding shard merge donor access blocker");
    stdx::lock_guard<Latch> lg(_mutex);
    tassert(8423342,
            "addShardMergeDonorAccessBlocker called with old-style multitenant migrations blocker",
            mtab->getProtocol() == MigrationProtocolEnum::kShardMerge);
    tassert(ErrorCodes::ConflictingOperationInProgress,
            str::stream() << "This node is already a shard merge donor",
            !_donorAccessBlocker);
    tassert(6114105,
            "Adding shard merge donor blocker when this node has other donor blockers",
            std::find_if(_tenantMigrationAccessBlockers.begin(),
                         _tenantMigrationAccessBlockers.end(),
                         [](const auto& pair) {
                             return pair.second.getAccessBlocker(MtabType::kDonor).get();
                         }) == _tenantMigrationAccessBlockers.end());
    _donorAccessBlocker = mtab;
}

void TenantMigrationAccessBlockerRegistry::_remove(WithLock, StringData tenantId, MtabType type) {
    auto it = _tenantMigrationAccessBlockers.find(tenantId);

    if (it == _tenantMigrationAccessBlockers.end()) {
        return;
    }

    auto& mtabPair = it->second;
    mtabPair.clearAccessBlocker(type);
    if (!mtabPair.getAccessBlocker(MtabType::kDonor) &&
        !mtabPair.getAccessBlocker(MtabType::kRecipient)) {
        _tenantMigrationAccessBlockers.erase(it);
    }
}

void TenantMigrationAccessBlockerRegistry::remove(StringData tenantId, MtabType type) {
    stdx::lock_guard<Latch> lg(_mutex);
    if (type == MtabType::kDonor && _donorAccessBlocker) {
        tasserted(8423348, "Using remove() for new-style donor access blocker");
    }

    _remove(lg, tenantId, type);
}

void TenantMigrationAccessBlockerRegistry::removeShardMergeDonorAccessBlocker(
    const UUID& migrationId) {
    stdx::lock_guard<Latch> lg(_mutex);
    if (_donorAccessBlocker && _donorAccessBlocker->getMigrationId() == migrationId) {
        // Shard merge has one donor blocker. If it exists it must be the one we're removing.
        _donorAccessBlocker->interrupt();
        _donorAccessBlocker.reset();
    }
}

void TenantMigrationAccessBlockerRegistry::removeRecipientAccessBlockersForMigration(
    const UUID& migrationId) {
    stdx::lock_guard<Latch> lg(_mutex);
    // Clear recipient blockers for migrationId, and erase pairs with no blocker remaining.
    erase_if(_tenantMigrationAccessBlockers,
             [&](std::pair<const std::string, DonorRecipientAccessBlockerPair> it) {
                 auto& mtabPair = it.second;
                 auto recipient = checked_pointer_cast<TenantMigrationRecipientAccessBlocker>(
                     mtabPair.getAccessBlocker(MtabType::kRecipient));
                 if (!recipient || recipient->getMigrationId() != migrationId) {
                     return false;
                 }

                 mtabPair.clearAccessBlocker(MtabType::kRecipient);
                 return !mtabPair.getAccessBlocker(MtabType::kDonor);
             });
}

void TenantMigrationAccessBlockerRegistry::removeAll(MtabType type) {
    stdx::lock_guard<Latch> lg(_mutex);

    for (auto it = _tenantMigrationAccessBlockers.begin();
         it != _tenantMigrationAccessBlockers.end();) {
        _remove(lg, (it++)->first, type);
    }

    if (_donorAccessBlocker) {
        _donorAccessBlocker->interrupt();
        _donorAccessBlocker.reset();
    }
}

boost::optional<MtabPair>
TenantMigrationAccessBlockerRegistry::getTenantMigrationAccessBlockerForDbName(StringData dbName) {
    stdx::lock_guard<Latch> lg(_mutex);
    return _getTenantMigrationAccessBlockersForDbName(dbName, lg);
}

std::shared_ptr<TenantMigrationAccessBlocker>
TenantMigrationAccessBlockerRegistry::getTenantMigrationAccessBlockerForDbName(StringData dbName,
                                                                               MtabType type) {
    stdx::lock_guard<Latch> lg(_mutex);
    auto mtabPair = _getTenantMigrationAccessBlockersForDbName(dbName, lg);
    if (!mtabPair) {
        return nullptr;
    }
    return mtabPair->getAccessBlocker(type);
}

boost::optional<MtabPair>
TenantMigrationAccessBlockerRegistry::_getTenantMigrationAccessBlockersForDbName(StringData dbName,
                                                                                 WithLock lk) {
    auto tenantId = tenant_migration_access_blocker::parseTenantIdFromDB(dbName).value_or("");
    auto it = _tenantMigrationAccessBlockers.find(tenantId);
    if (it != _tenantMigrationAccessBlockers.end()) {
        auto pair = it->second;
        if (_hasDonorAccessBlocker(lk, dbName)) {
            // I still have a recipient blocker from a recent migration, now I'm a donor.
            pair.setAccessBlocker(_donorAccessBlocker);
        }
        return pair;
    } else if (_hasDonorAccessBlocker(lk, dbName)) {
        return MtabPair(_donorAccessBlocker, nullptr);
    } else {
        return boost::none;
    }
}

bool TenantMigrationAccessBlockerRegistry::_hasDonorAccessBlocker(WithLock, StringData dbName) {
    // No-op oplog entries, e.g. for linearizable reads, use namespace "".
    bool isInternal = (dbName == "" || NamespaceString(dbName).isOnInternalDb());
    return _donorAccessBlocker && !isInternal;
}

std::shared_ptr<TenantMigrationAccessBlocker>
TenantMigrationAccessBlockerRegistry::getTenantMigrationAccessBlockerForTenantId(
    StringData tenantId, MtabType type) {
    stdx::lock_guard<Latch> lg(_mutex);
    if (type == MtabType::kDonor && _donorAccessBlocker) {
        return _donorAccessBlocker;
    }

    auto it = _tenantMigrationAccessBlockers.find(tenantId);
    if (it != _tenantMigrationAccessBlockers.end()) {
        return it->second.getAccessBlocker(type);
    } else {
        return nullptr;
    }
}

void TenantMigrationAccessBlockerRegistry::applyAll(
    TenantMigrationAccessBlocker::BlockerType type,
    const std::function<void(std::shared_ptr<TenantMigrationAccessBlocker>)>& callback) {
    stdx::lock_guard<Latch> lg(_mutex);
    for (auto& [tenantId, mtabPair] : _tenantMigrationAccessBlockers) {
        if (auto mtab = mtabPair.getAccessBlocker(type)) {
            callback(mtab);
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

    if (_donorAccessBlocker) {
        BSONObjBuilder donorMtabInfoBuilder;
        _donorAccessBlocker->appendInfoForServerStatus(&donorMtabInfoBuilder);
        builder->append("donor", donorMtabInfoBuilder.obj());
    }

    for (auto& [tenantId, mtabPair] : _tenantMigrationAccessBlockers) {
        BSONObjBuilder mtabInfoBuilder;

        if (auto donorMtab = mtabPair.getAccessBlocker(MtabType::kDonor)) {
            BSONObjBuilder donorMtabInfoBuilder;
            donorMtab->appendInfoForServerStatus(&donorMtabInfoBuilder);
            mtabInfoBuilder.append("donor", donorMtabInfoBuilder.obj());
        }

        if (auto recipientMtab = mtabPair.getAccessBlocker(MtabType::kRecipient)) {
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
        if (auto recipientMtab = mtabPair.getAccessBlocker(MtabType::kRecipient)) {
            recipientMtab->onMajorityCommitPointUpdate(opTime);
        }
        if (auto donorMtab = mtabPair.getAccessBlocker(MtabType::kDonor)) {
            donorMtab->onMajorityCommitPointUpdate(opTime);
        }
    }

    if (_donorAccessBlocker) {
        _donorAccessBlocker->onMajorityCommitPointUpdate(opTime);
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
