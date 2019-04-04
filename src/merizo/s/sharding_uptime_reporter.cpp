/**
 *    Copyright (C) 2018-present MerizoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MerizoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.merizodb.com/licensing/server-side-public-license>.
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

#define MERIZO_LOG_DEFAULT_COMPONENT ::merizo::logger::LogComponent::kSharding

#include "merizo/platform/basic.h"

#include "merizo/s/sharding_uptime_reporter.h"

#include "merizo/db/client.h"
#include "merizo/db/server_options.h"
#include "merizo/s/balancer_configuration.h"
#include "merizo/s/catalog/type_merizos.h"
#include "merizo/s/grid.h"
#include "merizo/util/concurrency/idle_thread_block.h"
#include "merizo/util/exit.h"
#include "merizo/util/log.h"
#include "merizo/util/merizoutils/str.h"
#include "merizo/util/net/hostname_canonicalization.h"
#include "merizo/util/net/socket_utils.h"
#include "merizo/util/version.h"

namespace merizo {
namespace {

const Seconds kUptimeReportInterval(10);

std::string constructInstanceIdString(const std::string& hostName) {
    return str::stream() << hostName << ":" << serverGlobalParams.port;
}

/**
 * Reports the uptime status of the current instance to the config.pings collection. This method
 * is best-effort and never throws.
 */
void reportStatus(OperationContext* opCtx,
                  const std::string& instanceId,
                  const std::string& hostName,
                  const Timer& upTimeTimer) {
    MerizosType mType;
    mType.setName(instanceId);
    mType.setPing(jsTime());
    mType.setUptime(upTimeTimer.seconds());
    // balancer is never active in merizos. Here for backwards compatibility only.
    mType.setWaiting(true);
    mType.setMerizoVersion(VersionInfoInterface::instance().version().toString());
    mType.setAdvisoryHostFQDNs(
        getHostFQDNs(hostName, HostnameCanonicalizationMode::kForwardAndReverse));

    try {
        Grid::get(opCtx)
            ->catalogClient()
            ->updateConfigDocument(opCtx,
                                   MerizosType::ConfigNS,
                                   BSON(MerizosType::name(instanceId)),
                                   BSON("$set" << mType.toBSON()),
                                   true,
                                   ShardingCatalogClient::kMajorityWriteConcern)
            .status_with_transitional_ignore();
    } catch (const std::exception& e) {
        log() << "Caught exception while reporting uptime: " << e.what();
    }
}

}  // namespace

ShardingUptimeReporter::ShardingUptimeReporter() = default;

ShardingUptimeReporter::~ShardingUptimeReporter() {
    // The thread must not be running when this object is destroyed
    invariant(!_thread.joinable());
}

void ShardingUptimeReporter::startPeriodicThread() {
    invariant(!_thread.joinable());

    _thread = stdx::thread([] {
        Client::initThread("Uptime-reporter");

        const std::string hostName(getHostNameCached());
        const std::string instanceId(constructInstanceIdString(hostName));
        const Timer upTimeTimer;

        while (!globalInShutdownDeprecated()) {
            {
                auto opCtx = cc().makeOperationContext();
                reportStatus(opCtx.get(), instanceId, hostName, upTimeTimer);

                auto status = Grid::get(opCtx.get())
                                  ->getBalancerConfiguration()
                                  ->refreshAndCheck(opCtx.get());
                if (!status.isOK()) {
                    warning() << "failed to refresh merizos settings" << causedBy(status);
                }
            }

            MERIZO_IDLE_THREAD_BLOCK;
            sleepFor(kUptimeReportInterval);
        }
    });
}


}  // namespace merizo
