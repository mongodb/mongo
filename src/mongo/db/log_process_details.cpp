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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kControl

#include "mongo/platform/basic.h"

#include "mongo/db/log_process_details.h"

#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_options_server_helpers.h"
#include "mongo/logv2/log.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/version.h"

namespace mongo {

bool is32bit() {
    return (sizeof(int*) == 4);
}

void logProcessDetails() {
    auto&& vii = VersionInfoInterface::instance();
    vii.logBuildInfo();

    ProcessInfo p;
    LOGV2(
        51765, "Operating system", "name"_attr = p.getOsName(), "version"_attr = p.getOsVersion());

    if (ProcessInfo::getMemSizeMB() < ProcessInfo::getSystemMemSizeMB()) {
        LOGV2_WARNING(20720,
                      "Available memory is less than system memory",
                      "availableMemSizeMB"_attr = ProcessInfo::getMemSizeMB(),
                      "systemMemSizeMB"_attr = ProcessInfo::getSystemMemSizeMB());
    }

    printCommandLineOpts();
}

void logProcessDetailsForLogRotate(ServiceContext* serviceContext) {
    LOGV2(20721,
          "Process Details",
          "pid"_attr = ProcessId::getCurrent(),
          "port"_attr = serverGlobalParams.port,
          "architecture"_attr = (is32bit() ? "32-bit" : "64-bit"),
          "host"_attr = getHostNameCached());

    auto replCoord = repl::ReplicationCoordinator::get(serviceContext);
    if (replCoord != nullptr &&
        replCoord->getReplicationMode() == repl::ReplicationCoordinator::modeReplSet) {
        auto rsConfig = replCoord->getConfig();

        if (rsConfig.isInitialized()) {
            LOGV2(20722,
                  "Replica Set Config",
                  "config"_attr = rsConfig,
                  "memberState"_attr = replCoord->getMemberState());
        } else {
            LOGV2(20724, "Node currently has no Replica Set Config.");
        }
    }

    logProcessDetails();
}

}  // namespace mongo
