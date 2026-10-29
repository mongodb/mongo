// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/log_process_details.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/bson/oid.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_options_server_helpers.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/process_id.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/version.h"

#include <ostream>
#include <string>
#include <string_view>

#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl


namespace mongo {
using namespace std::literals::string_view_literals;

namespace {

bool is32bit() {
    return (sizeof(int*) == 4);
}

void printCommandLineOpts(std::ostream* os) {
    if (os) {
        *os << fmt::format("Options set by command line: {}",
                           tojson(serverGlobalParams.parsedOpts, ExtendedRelaxedV2_0_0, true))
            << std::endl;
    } else {
        LOGV2(21951, "Options set by command line", "options"_attr = serverGlobalParams.parsedOpts);
    }
}

}  // namespace

void logProcessDetails(std::ostream* os) {
    auto&& vii = VersionInfoInterface::instance();
    if (ProcessInfo::getMemSizeMB() < ProcessInfo::getSystemMemSizeMB()) {
        LOGV2_WARNING(20720,
                      "Memory available to mongo process is less than total system memory",
                      "availableMemSizeMB"_attr = ProcessInfo::getMemSizeMB(),
                      "systemMemSizeMB"_attr = ProcessInfo::getSystemMemSizeMB());
    }
    auto osInfo = BSONObjBuilder()
                      .append("name", ProcessInfo::getOsName())
                      .append("version", ProcessInfo::getOsVersion())
                      .obj();
    vii.logBuildInfo(os);
    if (os) {
        *os << fmt::format("Operating System: {}", tojson(osInfo, ExtendedRelaxedV2_0_0, true))
            << std::endl;
    } else {
        LOGV2(51765, "Operating System", "os"_attr = osInfo);
    }
    printCommandLineOpts(os);
}

void logProcessDetailsForLogRotate(ServiceContext* serviceContext) {
    LOGV2(20721,
          "Process Details",
          "pid"_attr = ProcessId::getCurrent(),
          "port"_attr = serverGlobalParams.port,
          "architecture"_attr = (is32bit() ? "32-bit" : "64-bit"),
          "host"_attr = getHostNameCached());

    auto replCoord = repl::ReplicationCoordinator::get(serviceContext);
    if (replCoord != nullptr && replCoord->getSettings().isReplSet()) {
        auto rsConfig = replCoord->getConfig();

        if (rsConfig.isInitialized()) {
            LOGV2(20722,
                  "Node is a member of a replica set",
                  "config"_attr = rsConfig,
                  "memberState"_attr = replCoord->getMemberState());
        } else {
            LOGV2(20724, "Node currently has no replica set config");
        }
    }

    serverGlobalParams.featureCompatibility.acquireFCVSnapshot().logFCVWithContext(
        "log rotation"sv);
    logProcessDetails(nullptr);
}

}  // namespace mongo
