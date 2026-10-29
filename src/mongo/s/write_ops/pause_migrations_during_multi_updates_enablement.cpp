// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/write_ops/pause_migrations_during_multi_updates_enablement.h"

#include "mongo/db/server_parameter.h"
#include "mongo/db/topology/cluster_parameters/migration_blocking_operation_cluster_parameters_gen.h"
#include "mongo/s/transaction_router.h"
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
