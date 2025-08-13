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


#include "mongo/db/sharding_environment/router_uptime_reporter.h"

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bson_field.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/global_catalog/type_mongos.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/server_options.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/duration.h"
#include "mongo/util/exit.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/net/hostname_canonicalization.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"
#include "mongo/util/version.h"

#include <memory>
#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(disableShardingUptimeReporting);

const auto getRouterUptimeReporter = ServiceContext::declareDecoration<RouterUptimeReporter>();

const Seconds kUptimeReportInterval(10);

/**
 * Reports the uptime status of the current instance to the config.pings collection. This method
 * is best-effort and never throws.
 */
void reportStatus(OperationContext* opCtx,
                  const std::string& instanceId,
                  const std::string& hostName,
                  const Date_t& created,
                  const Timer& upTimeTimer) {
    if (MONGO_unlikely(disableShardingUptimeReporting.shouldFail())) {
        LOGV2(426322, "Disabling the reporting of the uptime status for the current instance.");
        return;
    }

    MongosType mType;
    mType.setName(instanceId);
    mType.setCreated(created);
    mType.setPing(Date_t::now());
    mType.setUptime(upTimeTimer.seconds());
    // balancer is never active in the router. Here for backwards compatibility only.
    mType.setWaiting(true);
    mType.setMongoVersion(std::string{VersionInfoInterface::instance().version()});
    auto statusWith = getHostFQDNs(hostName, HostnameCanonicalizationMode::kForwardAndReverse);
    if (statusWith.isOK()) {
        mType.setAdvisoryHostFQDNs(statusWith.getValue());
    }

    try {
        // Field "created" should not be updated every time.
        auto BSONObjForUpdate = mType.toBSON().removeField("created");
        BSONObjBuilder createdBSON;
        createdBSON.append("created", mType.getCreated());
        uassertStatusOK(Grid::get(opCtx)->catalogClient()->updateConfigDocument(
            opCtx,
            MongosType::ConfigNS,
            BSON(MongosType::name(instanceId)),
            BSON("$set" << BSONObjForUpdate << "$setOnInsert" << createdBSON.obj()),
            true,
            defaultMajorityWriteConcernDoNotUse()));
    } catch (const DBException& e) {
        LOGV2(22875,
              "Error while attempting to write this node's uptime to config.mongos",
              "error"_attr = e);
    }
}

}  // namespace

RouterUptimeReporter& RouterUptimeReporter::get(ServiceContext* serviceContext) {
    return getRouterUptimeReporter(serviceContext);
}

void RouterUptimeReporter::startPeriodicThread(ServiceContext* serviceContext) {
    invariant(!_thread.joinable());

    Date_t created = Date_t::now();

    _thread = stdx::thread([serviceContext, created] {
        // TODO(SERVER-74658): Please revisit if this thread could be made killable.
        Client::initThread("Uptime-reporter",
                           serviceContext->getService(ClusterRole::RouterServer),
                           Client::noSession(),
                           ClientOperationKillableByStepdown{false});

        auto opCtx = cc().makeOperationContext();
        const std::string hostName(getHostNameCached());
        const std::string instanceId(prettyHostNameAndPort(opCtx->getClient()->getLocalPort()));
        const Timer upTimeTimer;

        while (!globalInShutdownDeprecated()) {
            reportStatus(opCtx.get(), instanceId, hostName, created, upTimeTimer);

            auto status =
                Grid::get(opCtx.get())->getBalancerConfiguration()->refreshAndCheck(opCtx.get());
            if (!status.isOK()) {
                LOGV2_WARNING(22876,
                              "Failed to refresh balancer settings from config server",
                              "error"_attr = status);
            }

            // Refresh the router-role view of RWC defaults, which is always obtained from the
            // config server.
            try {
                ReadWriteConcernDefaults::get(opCtx.get()).refreshIfNecessary(opCtx.get());
            } catch (const DBException& ex) {
                LOGV2_WARNING(
                    22877,
                    "Failed to refresh readConcern/writeConcern defaults from config server",
                    "error"_attr = redact(ex));
            }

            MONGO_IDLE_THREAD_BLOCK;
            sleepFor(kUptimeReportInterval);
        }
    });
}


}  // namespace mongo
