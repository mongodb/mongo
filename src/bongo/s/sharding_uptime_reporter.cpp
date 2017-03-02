/**
 *    Copyright (C) 2016 BongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define BONGO_LOG_DEFAULT_COMPONENT ::bongo::logger::LogComponent::kSharding

#include "bongo/platform/basic.h"

#include "bongo/s/sharding_uptime_reporter.h"

#include "bongo/db/client.h"
#include "bongo/db/server_options.h"
#include "bongo/s/balancer_configuration.h"
#include "bongo/s/catalog/sharding_catalog_client.h"
#include "bongo/s/catalog/type_bongos.h"
#include "bongo/s/grid.h"
#include "bongo/util/exit.h"
#include "bongo/util/log.h"
#include "bongo/util/bongoutils/str.h"
#include "bongo/util/net/sock.h"
#include "bongo/util/version.h"

namespace bongo {
namespace {

const Seconds kUptimeReportInterval(10);

std::string constructInstanceIdString() {
    return str::stream() << getHostNameCached() << ":" << serverGlobalParams.port;
}

/**
 * Reports the uptime status of the current instance to the config.pings collection. This method
 * is best-effort and never throws.
 */
void reportStatus(OperationContext* txn, const std::string& instanceId, const Timer& upTimeTimer) {
    BongosType mType;
    mType.setName(instanceId);
    mType.setPing(jsTime());
    mType.setUptime(upTimeTimer.seconds());
    // balancer is never active in bongos. Here for backwards compatibility only.
    mType.setWaiting(true);
    mType.setBongoVersion(VersionInfoInterface::instance().version().toString());

    try {
        Grid::get(txn)->catalogClient(txn)->updateConfigDocument(
            txn,
            BongosType::ConfigNS,
            BSON(BongosType::name(instanceId)),
            BSON("$set" << mType.toBSON()),
            true,
            ShardingCatalogClient::kMajorityWriteConcern);
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

    _thread = stdx::thread([this] {
        Client::initThread("Uptime reporter");

        const std::string instanceId(constructInstanceIdString());
        const Timer upTimeTimer;

        while (!globalInShutdownDeprecated()) {
            {
                auto txn = cc().makeOperationContext();
                reportStatus(txn.get(), instanceId, upTimeTimer);

                auto status =
                    Grid::get(txn.get())->getBalancerConfiguration()->refreshAndCheck(txn.get());
                if (!status.isOK()) {
                    warning() << "failed to refresh bongos settings" << causedBy(status);
                }
            }

            sleepFor(kUptimeReportInterval);
        }
    });
}


}  // namespace bongo
