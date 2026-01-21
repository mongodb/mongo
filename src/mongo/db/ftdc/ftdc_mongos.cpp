/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/ftdc/ftdc_mongos.h"

#include "mongo/base/string_data.h"
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

void registerRouterCollectors(FTDCController* controller) {
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
