/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/s/write_ops/pause_migrations_during_multi_updates_enablement.h"

#include "mongo/db/server_parameter.h"
#include "mongo/s/migration_blocking_operation/migration_blocking_operation_cluster_parameters_gen.h"
#include "mongo/util/fail_point.h"

#include <random>

namespace mongo {

namespace {
MONGO_FAIL_POINT_DEFINE(returnRandomPauseMigrationsDuringMultiUpdatesParameter);

bool readPauseMigrationsEnablement() {
    auto* clusterParameters = ServerParameterSet::getClusterParameterSet();
    auto* clusterPauseMigrationsParam = clusterParameters->get<ClusterParameterWithStorage<
        migration_blocking_operation::PauseMigrationsDuringMultiUpdatesParam>>(
        "pauseMigrationsDuringMultiUpdates");
    auto clusterParameterEnabled = clusterPauseMigrationsParam->getValue(boost::none).getEnabled();

    returnRandomPauseMigrationsDuringMultiUpdatesParameter.execute([&](const auto& data) {
        std::mt19937 gen(time(nullptr));
        std::uniform_int_distribution<int> dist(0, 1);
        clusterParameterEnabled = (dist(gen) == 1);
    });

    return clusterParameterEnabled;
}
}  // namespace

bool PauseMigrationsDuringMultiUpdatesEnablement::isEnabled() {
    if (!_enabled.has_value()) {
        _enabled = readPauseMigrationsEnablement();
    }
    return _enabled.value();
}

}  // namespace mongo
