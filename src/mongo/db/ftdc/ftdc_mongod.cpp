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

#include "mongo/platform/basic.h"

#include "mongo/db/ftdc/ftdc_mongod.h"

#include <boost/filesystem.hpp>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/ftdc/constants.h"
#include "mongo/db/ftdc/controller.h"
#include "mongo/db/ftdc/ftdc_mongod_gen.h"
#include "mongo/db/ftdc/ftdc_server.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/util/assert_util.h"

namespace mongo {

Status validateCollectionStatsNamespaces(const std::vector<std::string> value,
                                         const boost::optional<TenantId>& tenantId) {
    try {
        for (const auto& nsStr : value) {
            NamespaceString ns(nsStr);

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
                NamespaceString ns(nsStr);
                auto result = CommandHelpers::runCommandDirectly(
                    opCtx,
                    OpMsgRequest::fromDBAndBody(
                        ns.db(), BSON("collStats" << ns.coll() << "waitForLock" << false)));
                builder.append(nsStr, result);

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
    if (repl::ReplicationCoordinator::get(getGlobalServiceContext())->getReplicationMode() !=
        repl::ReplicationCoordinator::modeNone) {
        // CmdReplSetGetStatus
        controller->addPeriodicCollector(std::make_unique<FTDCSimpleInternalCommandCollector>(
            "replSetGetStatus",
            "replSetGetStatus",
            "",
            BSON("replSetGetStatus" << 1 << "initialSync" << 0)));

        // CollectionStats
        controller->addPeriodicCollector(
            std::make_unique<FTDCSimpleInternalCommandCollector>("collStats",
                                                                 "local.oplog.rs.stats",
                                                                 "local",
                                                                 BSON("collStats"
                                                                      << "oplog.rs"
                                                                      << "waitForLock" << false
                                                                      << "numericOnly" << true)));
        if (serverGlobalParams.clusterRole != ClusterRole::ShardServer) {
            // GetDefaultRWConcern
            controller->addOnRotateCollector(std::make_unique<FTDCSimpleInternalCommandCollector>(
                "getDefaultRWConcern",
                "getDefaultRWConcern",
                "",
                BSON("getDefaultRWConcern" << 1 << "inMemory" << true)));
        }
    }

    controller->addPeriodicCollector(std::make_unique<FTDCCollectionStatsCollector>());
}

}  // namespace

void startMongoDFTDC() {
    auto dir = getFTDCDirectoryPathParameter();

    if (dir.empty()) {
        dir = storageGlobalParams.dbpath;
        dir /= kFTDCDefaultDirectory.toString();
    }

    startFTDC(dir, FTDCStartMode::kStart, registerMongoDCollectors);
}

void stopMongoDFTDC() {
    stopFTDC();
}

}  // namespace mongo
