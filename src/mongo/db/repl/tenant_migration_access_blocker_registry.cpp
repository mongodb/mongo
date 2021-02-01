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
#include "mongo/db/repl/tenant_migration_donor_access_blocker.h"

namespace mongo {

const ServiceContext::Decoration<TenantMigrationAccessBlockerRegistry>
    TenantMigrationAccessBlockerRegistry::get =
        ServiceContext::declareDecoration<TenantMigrationAccessBlockerRegistry>();

void TenantMigrationAccessBlockerRegistry::add(StringData tenantId,
                                               std::shared_ptr<TenantMigrationAccessBlocker> mtab) {
    stdx::lock_guard<Latch> lg(_mutex);

    // Assume that all tenant ids (i.e. 'tenantId') have equal length.
    auto it = _tenantMigrationAccessBlockers.find(tenantId);

    if (it != _tenantMigrationAccessBlockers.end()) {
        uasserted(ErrorCodes::ConflictingOperationInProgress,
                  str::stream() << "Found active migration for tenantId \"" << it->first
                                << "\" which conflicts with the specified tenantId \"" << tenantId
                                << "\"");
    }

    _tenantMigrationAccessBlockers.emplace(tenantId, mtab);
}

void TenantMigrationAccessBlockerRegistry::remove(StringData tenantId) {
    stdx::lock_guard<Latch> lg(_mutex);

    auto it = _tenantMigrationAccessBlockers.find(tenantId);
    invariant(it != _tenantMigrationAccessBlockers.end());

    _tenantMigrationAccessBlockers.erase(it);
}

std::shared_ptr<TenantMigrationAccessBlocker>
TenantMigrationAccessBlockerRegistry::getTenantMigrationAccessBlockerForDbName(StringData dbName) {
    stdx::lock_guard<Latch> lg(_mutex);

    auto it = std::find_if(
        _tenantMigrationAccessBlockers.begin(),
        _tenantMigrationAccessBlockers.end(),
        [dbName](
            const std::pair<std::string, std::shared_ptr<TenantMigrationAccessBlocker>>& blocker) {
            StringData tenantId = blocker.first;
            return dbName.startsWith(tenantId + "_");
        });

    if (it == _tenantMigrationAccessBlockers.end()) {
        return nullptr;
    } else {
        return it->second;
    }
}

std::shared_ptr<TenantMigrationAccessBlocker>
TenantMigrationAccessBlockerRegistry::getTenantMigrationAccessBlockerForTenantId(
    StringData tenantId) {
    stdx::lock_guard<Latch> lg(_mutex);

    auto it = _tenantMigrationAccessBlockers.find(tenantId);
    if (it != _tenantMigrationAccessBlockers.end()) {
        return it->second;
    } else {
        return nullptr;
    }
}

void TenantMigrationAccessBlockerRegistry::shutDown() {
    stdx::lock_guard<Latch> lg(_mutex);
    _tenantMigrationAccessBlockers.clear();
}

void TenantMigrationAccessBlockerRegistry::appendInfoForServerStatus(BSONObjBuilder* builder) {
    stdx::lock_guard<Latch> lg(_mutex);

    std::for_each(
        _tenantMigrationAccessBlockers.begin(),
        _tenantMigrationAccessBlockers.end(),
        [builder](
            const std::pair<std::string, std::shared_ptr<TenantMigrationAccessBlocker>>& blocker) {
            blocker.second->appendInfoForServerStatus(builder);
        });
}

void TenantMigrationAccessBlockerRegistry::onMajorityCommitPointUpdate(repl::OpTime opTime) {
    stdx::lock_guard<Latch> lg(_mutex);

    std::for_each(
        _tenantMigrationAccessBlockers.begin(),
        _tenantMigrationAccessBlockers.end(),
        [opTime](
            const std::pair<std::string, std::shared_ptr<TenantMigrationAccessBlocker>>& blocker) {
            blocker.second->onMajorityCommitPointUpdate(opTime);
        });
}

}  // namespace mongo
