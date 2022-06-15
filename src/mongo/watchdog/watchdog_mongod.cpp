/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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


#include "mongo/platform/basic.h"

#include "mongo/watchdog/watchdog_mongod.h"

#include <boost/filesystem.hpp>
#include <memory>

#include "mongo/base/init.h"
#include "mongo/config.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/logv2/log.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/tick_source_mock.h"
#include "mongo/watchdog/watchdog.h"
#include "mongo/watchdog/watchdog_mongod_gen.h"
#include "mongo/watchdog/watchdog_register.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo {
namespace {

// Run the watchdog checks at a fixed interval regardless of user choice for monitoring period.
constexpr Seconds watchdogCheckPeriod = Seconds{10};

const auto getWatchdogMonitor =
    ServiceContext::declareDecoration<std::unique_ptr<WatchdogMonitor>>();

// A boolean variable to track whether the watchdog was enabled at startup.
// Defaults to true because set parameters are handled before we start the watchdog if needed.
bool watchdogEnabled{true};

WatchdogMonitor* getGlobalWatchdogMonitor() {
    if (!hasGlobalServiceContext()) {
        return nullptr;
    }

    return getWatchdogMonitor(getGlobalServiceContext()).get();
}

}  // namespace

Status validateWatchdogPeriodSeconds(const int& value) {
    if (value < 60 && value != -1) {

        return {ErrorCodes::BadValue, "watchdogPeriodSeconds must be greater than or equal to 60s"};
    }

    // If the watchdog was not enabled at startup, disallow changes the period.
    if (!watchdogEnabled) {
        return {ErrorCodes::BadValue,
                "watchdogPeriodSeconds cannot be changed at runtime if it was not set at startup"};
    }

    return Status::OK();
}

Status onUpdateWatchdogPeriodSeconds(const int& value) {
    auto monitor = getGlobalWatchdogMonitor();
    if (monitor) {
        monitor->setPeriod(Seconds(value));
    }

    return Status::OK();
}

/**
 * Server status section for the Watchdog.
 *
 * Sample format:
 *
 * watchdog: {
 *       generation: int,
 * }
 */
class WatchdogServerStatusSection : public ServerStatusSection {
public:
    WatchdogServerStatusSection() : ServerStatusSection("watchdog") {}
    bool includeByDefault() const {
        // Only include this by default if the watchdog is on
        return watchdogEnabled;
    }

    BSONObj generateSection(OperationContext* opCtx, const BSONElement& configElement) const {
        if (!watchdogEnabled) {
            return BSONObj();
        }

        BSONObjBuilder result;
        WatchdogMonitor* watchdog = getWatchdogMonitor(opCtx->getServiceContext()).get();
        invariant(watchdog);

        result.append("checkGeneration", watchdog->getCheckGeneration());
        result.append("monitorGeneration", watchdog->getMonitorGeneration());
        result.append("monitorPeriod", gWatchdogPeriodSeconds.load());

        return result.obj();
    }
} watchdogServerStatusSection;

void startWatchdog(ServiceContext* service) {
    // Check three paths if set
    // 1. storage directory - optional for inmemory?
    // 2. log path - optional
    // 3. audit path - optional

    Seconds period{gWatchdogPeriodSeconds.load()};
    if (period < Seconds::zero()) {
        // Skip starting the watchdog if the user has not asked for it.
        watchdogEnabled = false;
        return;
    }

    watchdogEnabled = true;

    std::vector<std::unique_ptr<WatchdogCheck>> checks;

    auto dataCheck =
        std::make_unique<DirectoryCheck>(boost::filesystem::path(storageGlobalParams.dbpath));

    checks.push_back(std::move(dataCheck));

    // Check for the journal.
    auto journalDirectory = boost::filesystem::path(storageGlobalParams.dbpath);
    journalDirectory /= "journal";

    if (boost::filesystem::exists(journalDirectory)) {
        auto journalCheck = std::make_unique<DirectoryCheck>(journalDirectory);

        checks.push_back(std::move(journalCheck));
    } else {
        LOGV2_WARNING(23835,
                      "Watchdog is skipping check for journal directory since it does not "
                      "exist: '{journalDirectory_generic_string}'",
                      "journalDirectory_generic_string"_attr = journalDirectory.generic_string());
    }

    // If the user specified a log path, also monitor that directory.
    // This may be redudant with the dbpath check but there is not easy way to confirm they are
    // duplicate.
    if (!serverGlobalParams.logpath.empty()) {
        boost::filesystem::path logFile(serverGlobalParams.logpath);
        auto logPath = logFile.parent_path();

        auto logCheck = std::make_unique<DirectoryCheck>(logPath);
        checks.push_back(std::move(logCheck));
    }

    // If the user specified an audit path, also monitor that directory.
    // This may be redudant with the dbpath check but there is not easy way to confirm they are
    // duplicate.
    for (auto&& path : getWatchdogPaths()) {
        auto auditCheck = std::make_unique<DirectoryCheck>(path);
        checks.push_back(std::move(auditCheck));
    }

    auto monitor = std::make_unique<WatchdogMonitor>(
        std::move(checks), watchdogCheckPeriod, period, watchdogTerminate);

    // Install the new WatchdogMonitor
    auto& staticMonitor = getWatchdogMonitor(service);

    staticMonitor = std::move(monitor);

    staticMonitor->start();
}

}  // namespace mongo
