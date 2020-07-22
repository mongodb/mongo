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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication

#include "mongo/db/repl/migrating_tenant_access_blocker_by_prefix.h"
#include "mongo/db/repl/migrating_tenant_access_blocker.h"

namespace mongo {

const ServiceContext::Decoration<MigratingTenantAccessBlockerByPrefix>
    MigratingTenantAccessBlockerByPrefix::get =
        ServiceContext::declareDecoration<MigratingTenantAccessBlockerByPrefix>();

/**
 * Invariants that no entry for dbPrefix exists and then adds the entry for (dbPrefix, mtab)
 */
void MigratingTenantAccessBlockerByPrefix::add(StringData dbPrefix,
                                               std::shared_ptr<MigratingTenantAccessBlocker> mtab) {
    stdx::lock_guard<Latch> lg(_mutex);

    auto it = _migratingTenantAccessBlockers.find(dbPrefix);
    invariant(it == _migratingTenantAccessBlockers.end());

    _migratingTenantAccessBlockers.emplace(dbPrefix, mtab);
}


/**
 * Invariants that an entry for dbPrefix exists, and then removes the entry for (dbPrefix, mtab)
 */
void MigratingTenantAccessBlockerByPrefix::remove(StringData dbPrefix) {
    stdx::lock_guard<Latch> lg(_mutex);

    auto it = _migratingTenantAccessBlockers.find(dbPrefix);
    invariant(it != _migratingTenantAccessBlockers.end());

    _migratingTenantAccessBlockers.erase(it);
}


/**
 * Iterates through each of the MigratingTenantAccessBlockers and
 * returns the first MigratingTenantBlocker it finds whose dbPrefix is a prefix for dbName.
 */
std::shared_ptr<MigratingTenantAccessBlocker>
MigratingTenantAccessBlockerByPrefix::getMigratingTenantBlocker(StringData dbName) {
    stdx::lock_guard<Latch> lg(_mutex);

    auto doesDBNameStartWithPrefix =
        [dbName](
            const std::pair<std::string, std::shared_ptr<MigratingTenantAccessBlocker>>& blocker) {
            StringData dbPrefix = blocker.first;
            return dbName.startsWith(dbPrefix);
        };

    auto it = std::find_if(_migratingTenantAccessBlockers.begin(),
                           _migratingTenantAccessBlockers.end(),
                           doesDBNameStartWithPrefix);

    if (it == _migratingTenantAccessBlockers.end()) {
        return nullptr;
    } else {
        return it->second;
    }
}

/**
 * Iterates through each of the MigratingTenantAccessBlockers stored by the mapping
 * and appends the server status of each blocker to the BSONObjBuilder.
 */
void MigratingTenantAccessBlockerByPrefix::appendInfoForServerStatus(BSONObjBuilder* builder) {

    auto appendBlockerStatus =
        [builder](
            const std::pair<std::string, std::shared_ptr<MigratingTenantAccessBlocker>>& blocker) {
            BSONObjBuilder tenantBuilder;
            blocker.second->appendInfoForServerStatus(&tenantBuilder);
            builder->append(blocker.first, tenantBuilder.obj());
        };

    std::for_each(_migratingTenantAccessBlockers.begin(),
                  _migratingTenantAccessBlockers.end(),
                  appendBlockerStatus);
}

}  // namespace mongo