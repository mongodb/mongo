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

namespace mongo {
using MtabType = TenantMigrationAccessBlocker::BlockerType;
using MtabPair = TenantMigrationAccessBlockerRegistry::DonorRecipientAccessBlockerPair;

const ServiceContext::Decoration<TenantMigrationAccessBlockerRegistry>
    TenantMigrationAccessBlockerRegistry::get =
        ServiceContext::declareDecoration<TenantMigrationAccessBlockerRegistry>();

void TenantMigrationAccessBlockerRegistry::add(StringData tenantId,
                                               std::shared_ptr<TenantMigrationAccessBlocker> mtab) {
    stdx::lock_guard<Latch> lg(_mutex);
    auto mtabType = mtab->getType();
    // Assume that all tenant ids (i.e. 'tenantId') have equal length.
    auto it = _tenantMigrationAccessBlockers.find(tenantId);
    if (it != _tenantMigrationAccessBlockers.end()) {
        if (it->second.getAccessBlocker(mtabType)) {
            uasserted(ErrorCodes::ConflictingOperationInProgress,
                      str::stream()
                          << "Found active migration for tenantId \"" << tenantId << "\"");
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
    _remove(lg, tenantId, type);
}

void TenantMigrationAccessBlockerRegistry::removeAll(MtabType type) {
    stdx::lock_guard<Latch> lg(_mutex);

    for (auto it = _tenantMigrationAccessBlockers.begin();
         it != _tenantMigrationAccessBlockers.end();) {
        _remove(lg, (it++)->first, type);
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
                                                                                 WithLock) {
    auto it = std::find_if(_tenantMigrationAccessBlockers.begin(),
                           _tenantMigrationAccessBlockers.end(),
                           [dbName](const std::pair<std::string, MtabPair>& blocker) {
                               StringData tenantId = blocker.first;
                               return dbName.startsWith(tenantId + "_");
                           });

    if (it == _tenantMigrationAccessBlockers.end()) {
        return boost::none;
    } else {
        return it->second;
    }
}

std::shared_ptr<TenantMigrationAccessBlocker>
TenantMigrationAccessBlockerRegistry::getTenantMigrationAccessBlockerForTenantId(
    StringData tenantId, MtabType type) {
    stdx::lock_guard<Latch> lg(_mutex);

    auto it = _tenantMigrationAccessBlockers.find(tenantId);
    if (it != _tenantMigrationAccessBlockers.end()) {
        return it->second.getAccessBlocker(type);
    } else {
        return nullptr;
    }
}

void TenantMigrationAccessBlockerRegistry::shutDown() {
    stdx::lock_guard<Latch> lg(_mutex);
    _tenantMigrationAccessBlockers.clear();
}

void TenantMigrationAccessBlockerRegistry::appendInfoForServerStatus(
    BSONObjBuilder* builder) const {
    stdx::lock_guard<Latch> lg(_mutex);

    for (auto& [tenantId, mtabPair] : _tenantMigrationAccessBlockers) {
        BSONObjBuilder mtabInfoBuilder;
        if (auto recipientMtab = mtabPair.getAccessBlocker(MtabType::kRecipient)) {
            recipientMtab->appendInfoForServerStatus(&mtabInfoBuilder);
        }
        if (auto donorMtab = mtabPair.getAccessBlocker(MtabType::kDonor)) {
            donorMtab->appendInfoForServerStatus(&mtabInfoBuilder);
        }

        builder->append(tenantId, mtabInfoBuilder.obj());
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
}

}  // namespace mongo
