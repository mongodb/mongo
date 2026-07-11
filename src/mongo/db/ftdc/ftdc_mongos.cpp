// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/ftdc/ftdc_mongos.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/replica_set_monitor_manager.h"
#include "mongo/db/ftdc/ftdc_server.h"
#include "mongo/db/ftdc/networking_collectors.h"
#include "mongo/db/ftdc/util.h"
#include "mongo/db/server_options.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/executor/connection_pool_stats.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/logv2/log.h"

#include <functional>
#include <memory>
#include <string>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kFTDC


namespace mongo {

namespace {

void registerRouterCollectors(ServiceContext*, FTDCController* controller) {
    registerServerCollectors(controller);

    registerNetworkingCollectors(controller);

    controller->addPeriodicMetadataCollector(std::make_unique<FTDCSimpleInternalCommandCollector>(
        "getParameter",
        "getParameter",
        DatabaseName::kEmpty,
        BSON("getParameter" << BSON("allParameters" << true << "setAt"
                                                    << "runtime"))));

    controller->addPeriodicMetadataCollector(std::make_unique<FTDCSimpleInternalCommandCollector>(
        "getClusterParameter",
        "getClusterParameter",
        DatabaseName::kEmpty,
        BSON("getClusterParameter" << "*"
                                   << "omitInFTDC" << true)));
}

}  // namespace

void startMongoSFTDC(ServiceContext* serviceContext) {
    // Get the path to use for FTDC:
    // 1. Check if the user set one.
    // 2. If not, check if the user has a logpath and derive one.
    // 3. Otherwise, tell the user FTDC cannot run.

    // Only attempt to enable FTDC if we have a path to log files to.
    FTDCStartMode startMode = FTDCStartMode::kStart;
    auto directory = getFTDCDirectoryPathParameter();

    if (directory.empty()) {
        if (serverGlobalParams.logpath.empty()) {
            LOGV2_WARNING(23911,
                          "FTDC is disabled because neither '--logpath' nor set parameter "
                          "'diagnosticDataCollectionDirectoryPath' are specified.");
            startMode = FTDCStartMode::kSkipStart;
        } else {
            directory = boost::filesystem::absolute(
                FTDCUtil::getMongoSPath(serverGlobalParams.logpath), serverGlobalParams.cwd);

            // Note: If the computed FTDC directory conflicts with an existing file, then FTDC will
            // warn about the conflict, and not startup. It will not terminate MongoS in this
            // situation.
        }
    }

    startFTDC(serviceContext, directory, startMode, {registerRouterCollectors});
}

void stopMongoSFTDC() {
    stopFTDC();
}

}  // namespace mongo
