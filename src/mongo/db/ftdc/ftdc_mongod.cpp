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

#include <boost/filesystem/path.hpp>
#include <fmt/format.h>
#include <memory>

#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/cluster_role.h"
#include "mongo/db/commands.h"
#include "mongo/db/ftdc/collector.h"
#include "mongo/db/ftdc/constants.h"
#include "mongo/db/ftdc/controller.h"
#include "mongo/db/ftdc/ftdc_mongod.h"
#include "mongo/db/ftdc/ftdc_mongod_gen.h"
#include "mongo/db/ftdc/ftdc_server.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/transport/transport_layer_ftdc_collector.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/synchronized_value.h"

namespace mongo {

Status validateCollectionStatsNamespaces(const std::vector<std::string> value,
                                         const boost::optional<TenantId>& tenantId) {
    try {
        for (const auto& nsStr : value) {
            const auto ns = NamespaceStringUtil::deserialize(
                tenantId, nsStr, SerializationContext::stateDefault());

            if (!ns.isValid()) {
                return Status(ErrorCodes::BadValue,
                              fmt::format("'{}' is not a valid namespace", nsStr));
            }
        }
    } catch (...) {
        return exceptionToStatus();
    }

    return Status::OK();
}

namespace {

class FTDCCollectionStatsCollector final : public FTDCCollectorInterface {
public:
    bool hasData() const override {
        return !gDiagnosticDataCollectionStatsNamespaces->empty();
    }

    void collect(OperationContext* opCtx, BSONObjBuilder& builder) override {
        std::vector<std::string> namespaces = gDiagnosticDataCollectionStatsNamespaces.get();

        for (const auto& nsStr : namespaces) {

            try {
                // TODO SERVER-74464 tenantId needs to be passed.
                const auto ns =
                    NamespaceStringUtil::parseFromStringExpectTenantIdInMultitenancyMode(nsStr);
                auto result = CommandHelpers::runCommandDirectly(
                    opCtx,
                    OpMsgRequestBuilder::create(
                        auth::ValidatedTenancyScope::get(opCtx),
                        ns.dbName(),
                        BSON("aggregate" << ns.coll() << "cursor" << BSONObj{} << "pipeline"
                                         << BSON_ARRAY(BSON("$collStats" << BSON(
                                                                "storageStats" << BSON(
                                                                    "waitForLock" << false)))))));
                builder.append(nsStr, result["cursor"]["firstBatch"]["0"].Obj());

            } catch (...) {
                Status s = exceptionToStatus();
                builder.append("error", s.toString());
            }
        }
    }

    std::string name() const override {
        return "collectionStats";
    }
};


void registerMongoDCollectors(FTDCController* controller) {
    // These metrics are only collected if replication is enabled
    if (repl::ReplicationCoordinator::get(getGlobalServiceContext())->getSettings().isReplSet()) {
        // CmdReplSetGetStatus
        controller->addPeriodicCollector(std::make_unique<FTDCSimpleInternalCommandCollector>(
                                             "replSetGetStatus",
                                             "replSetGetStatus",
                                             DatabaseName::kEmpty,
                                             BSON("replSetGetStatus" << 1 << "initialSync" << 0)),
                                         ClusterRole::ShardServer);

        // CollectionStats
        controller->addPeriodicCollector(
            std::make_unique<FTDCSimpleInternalCommandCollector>(
                "aggregate",
                "local.oplog.rs.stats",
                DatabaseName::kLocal,
                BSON("aggregate"
                     << "oplog.rs"
                     << "cursor" << BSONObj{} << "pipeline"
                     << BSON_ARRAY(BSON(
                            "$collStats"
                            << BSON("storageStats"
                                    << BSON("waitForLock" << false << "numericOnly" << true)))))),
            ClusterRole::ShardServer);
        if (!serverGlobalParams.clusterRole.hasExclusively(ClusterRole::ShardServer)) {
            // GetDefaultRWConcern
            controller->addOnRotateCollector(
                std::make_unique<FTDCSimpleInternalCommandCollector>(
                    "getDefaultRWConcern",
                    "getDefaultRWConcern",
                    DatabaseName::kEmpty,
                    BSON("getDefaultRWConcern" << 1 << "inMemory" << true)),
                ClusterRole::None);
        }
    }

    controller->addPeriodicCollector(std::make_unique<FTDCCollectionStatsCollector>(),
                                     ClusterRole::ShardServer);

    controller->addPeriodicCollector(std::make_unique<transport::TransportLayerFTDCCollector>(),
                                     ClusterRole::ShardServer);
}

}  // namespace

void startMongoDFTDC(ServiceContext* serviceContext) {
    auto dir = getFTDCDirectoryPathParameter();

    if (dir.empty()) {
        dir = storageGlobalParams.dbpath;
        dir /= kFTDCDefaultDirectory.toString();
    }

    startFTDC(serviceContext->getService(ClusterRole::ShardServer),
              dir,
              FTDCStartMode::kStart,
              registerMongoDCollectors);
}

void stopMongoDFTDC(ServiceContext* serviceContext) {
    stopFTDC(serviceContext->getService(ClusterRole::ShardServer));
}

}  // namespace mongo
