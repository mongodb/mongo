/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/sharding_uptime_reporter.h"

#include "mongo/db/client.h"
#include "mongo/db/server_options.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_mongos.h"
#include "mongo/s/grid.h"
#include "mongo/util/exit.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/sock.h"
#include "mongo/util/version.h"

namespace mongo {
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
    MongosType mType;
    mType.setName(instanceId);
    mType.setPing(jsTime());
    mType.setUptime(upTimeTimer.seconds());
    // balancer is never active in mongos. Here for backwards compatibility only.
    mType.setWaiting(true);
    mType.setMongoVersion(VersionInfoInterface::instance().version().toString());

    try {
        Grid::get(txn)->catalogClient(txn)->updateConfigDocument(
            txn,
            MongosType::ConfigNS,
            BSON(MongosType::name(instanceId)),
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

        while (!inShutdown()) {
            {
                auto txn = cc().makeOperationContext();
                reportStatus(txn.get(), instanceId, upTimeTimer);

                auto status =
                    Grid::get(txn.get())->getBalancerConfiguration()->refreshAndCheck(txn.get());
                if (!status.isOK()) {
                    warning() << "failed to refresh mongos settings" << causedBy(status);
                }
            }

            sleepFor(kUptimeReportInterval);
        }
    });
}


}  // namespace mongo
