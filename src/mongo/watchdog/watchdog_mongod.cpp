// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/watchdog/watchdog_mongod.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/commands/server_status/server_status.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/testing_proctor.h"
#include "mongo/watchdog/watchdog.h"
#include "mongo/watchdog/watchdog_mongod_gen.h"
#include "mongo/watchdog/watchdog_register.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo {
namespace {

// Run the watchdog checks at a fixed interval regardless of user choice for monitoring period.
constexpr Seconds watchdogCheckPeriod = Seconds{10};

// A boolean variable to track whether the watchdog was enabled at startup.
// Defaults to true because set parameters are handled before we start the watchdog if needed.
bool watchdogEnabled{true};

}  // namespace

Status validateWatchdogPeriodSeconds(const int& value, const boost::optional<TenantId>&) {
    const bool shouldSkipValidateForTest =
        TestingProctor::instance().isInitialized() && TestingProctor::instance().isEnabled();
    if (!shouldSkipValidateForTest && value < 60 && value != -1) {
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
    auto monitor = WatchdogMonitorInterface::getGlobalWatchdogMonitorInterface();
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
    using ServerStatusSection::ServerStatusSection;
    bool includeByDefault() const override {
        // Only include this by default if the watchdog is on
        return watchdogEnabled;
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        if (!watchdogEnabled) {
            return BSONObj();
        }

        BSONObjBuilder result;
        WatchdogMonitorInterface* watchdog =
            WatchdogMonitorInterface::get(opCtx->getServiceContext());
        invariant(watchdog);

        result.append("checkGeneration", watchdog->getCheckGeneration());
        result.append("monitorGeneration", watchdog->getMonitorGeneration());
        result.append("monitorPeriod", gWatchdogPeriodSeconds.load());

        return result.obj();
    }
};
auto& watchdogServerStatusSection =
    *ServerStatusSectionBuilder<WatchdogServerStatusSection>("watchdog").forShard();

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

    WatchdogMonitorInterface::set(
        service,
        std::make_unique<WatchdogMonitor>(
            std::move(checks), watchdogCheckPeriod, period, watchdogTerminate));

    // Install the new WatchdogMonitor
    auto staticMonitor = WatchdogMonitorInterface::get(service);
    staticMonitor->start();
}

}  // namespace mongo
