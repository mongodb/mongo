/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/repl/tenant_migration_statistics.h"

namespace mongo {

// static
const ServiceContext::Decoration<TenantMigrationStatistics> statisticsDecoration =
    ServiceContext::declareDecoration<TenantMigrationStatistics>();

// static
TenantMigrationStatistics* TenantMigrationStatistics::get(ServiceContext* service) {
    return &statisticsDecoration(service);
}

std::unique_ptr<ScopeGuard<std::function<void()>>>
TenantMigrationStatistics::getScopedOutstandingDonatingCount() {
    _currentMigrationsDonating.fetchAndAddRelaxed(1);
    return std::make_unique<ScopeGuard<std::function<void()>>>(
        [this] { _currentMigrationsDonating.fetchAndAddRelaxed(-1); });
}

std::unique_ptr<ScopeGuard<std::function<void()>>>
TenantMigrationStatistics::getScopedOutstandingReceivingCount() {
    _currentMigrationsReceiving.fetchAndAddRelaxed(1);
    return std::make_unique<ScopeGuard<std::function<void()>>>(
        [this] { _currentMigrationsReceiving.fetchAndAddRelaxed(-1); });
}

void TenantMigrationStatistics::incTotalSuccessfulMigrationsDonated() {
    _totalSuccessfulMigrationsDonated.fetchAndAddRelaxed(1);
}

void TenantMigrationStatistics::incTotalSuccessfulMigrationsReceived() {
    _totalSuccessfulMigrationsReceived.fetchAndAddRelaxed(1);
}

void TenantMigrationStatistics::incTotalFailedMigrationsDonated() {
    _totalFailedMigrationsDonated.fetchAndAddRelaxed(1);
}

void TenantMigrationStatistics::incTotalFailedMigrationsReceived() {
    _totalFailedMigrationsReceived.fetchAndAddRelaxed(1);
}

void TenantMigrationStatistics::appendInfoForServerStatus(BSONObjBuilder* builder) const {
    builder->append("currentMigrationsDonating", _currentMigrationsDonating.load());
    builder->append("currentMigrationsReceiving", _currentMigrationsReceiving.load());
    builder->append("totalSuccessfulMigrationsDonated", _totalSuccessfulMigrationsDonated.load());
    builder->append("totalSuccessfulMigrationsReceived", _totalSuccessfulMigrationsReceived.load());
    builder->append("totalFailedMigrationsDonated", _totalFailedMigrationsDonated.load());
    builder->append("totalFailedMigrationsReceived", _totalFailedMigrationsReceived.load());
}

}  // namespace mongo
