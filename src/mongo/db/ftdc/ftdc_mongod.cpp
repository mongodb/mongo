// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/ftdc/ftdc_mongod.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands.h"
#include "mongo/db/ftdc/collector.h"
#include "mongo/db/ftdc/constants.h"
#include "mongo/db/ftdc/controller.h"
#include "mongo/db/ftdc/ftdc_mongod_gen.h"
#include "mongo/db/ftdc/ftdc_mongos.h"
#include "mongo/db/ftdc/ftdc_server.h"
#include "mongo/db/ftdc/networking_collectors.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/synchronized_value.h"

#include <memory>
#include <string_view>

#include <boost/filesystem/path.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

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
using namespace std::literals::string_view_literals;

struct CollectionStatsSpec {
    std::string_view statsName;
    std::string_view collName;
    const DatabaseName& dbName;
};

std::vector<CollectionStatsSpec> getCollectionSpecs(ServiceContext* serviceContext) {
    std::vector<CollectionStatsSpec> specs{
        {"local.oplog.rs.stats"sv, "oplog.rs"sv, DatabaseName::kLocal},
    };

    auto& rss = rss::ReplicatedStorageService::get(serviceContext);
    if (!rss.getPersistenceProvider().shouldUseReplicatedFastCount()) {
        specs.emplace_back("config.transactions.stats"sv, "transactions"sv, DatabaseName::kConfig);
    }

    if (rss.getPersistenceProvider().supportsFindAndModifyImageCollection()) {
        specs.emplace_back(
            "config.image_collection.stats"sv, "image_collection"sv, DatabaseName::kConfig);
    }

    return specs;
}

class FTDCCollectionStatsCollector final : public FTDCCollectorInterface {
public:
    bool hasData() const override {
        return !gDiagnosticDataCollectionStatsNamespaces->empty();
    }

    void collect(OperationContext* opCtx, BSONObjBuilder& builder) override {
        std::vector<std::string> namespaces = gDiagnosticDataCollectionStatsNamespaces.get();

        auto ru = shard_role_details::getRecoveryUnit(opCtx);
        if (ru) {
            // Prevent FTDC from getting blocked on cache eviction.
            ru->optOutOfCacheEviction();
        }

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

static const BSONArray pipelineObj =
    BSONArrayBuilder{}
        .append(BSONObjBuilder{}
                    .append("$collStats",
                            BSONObjBuilder{}
                                .append("storageStats",
                                        BSONObjBuilder{}
                                            .append("waitForLock", false)
                                            .append("numericOnly", true)
                                            .obj())
                                .obj())
                    .obj())
        .arr();

static const BSONObj getParameterQueryObj =
    BSONObjBuilder{}
        .append("getParameter",
                BSONObjBuilder{}.append("allParameters", true).append("setAt", "runtime").obj())
        .obj();

static const BSONObj getClusterParameterQueryObj =
    BSONObjBuilder{}.append("getClusterParameter", "*").append("omitInFTDC", true).obj();

static const BSONObj replSetGetStatusObj =
    BSONObjBuilder{}.append("replSetGetStatus", 1).append("initialSync", 2).obj();

repl::ReplicationCoordinator* getGlobalRC() {
    return repl::ReplicationCoordinator::get(getGlobalServiceContext());
}

bool isRepl(const repl::ReplicationCoordinator& rc) {
    return rc.getSettings().isReplSet();
}

bool isArbiter(const repl::ReplicationCoordinator& rc) {
    return isRepl(rc) && rc.getMemberState().arbiter();
}

bool isDataStoringNode() {
    auto rc = getGlobalRC();
    return !(rc && isArbiter(*rc));
}

std::unique_ptr<FTDCCollectorInterface> makeFilteredCollector(
    std::function<bool()> pred, std::unique_ptr<FTDCCollectorInterface> collector) {
    return std::make_unique<FilteredFTDCCollector>(std::move(pred), std::move(collector));
}

void registerShardCollectors(ServiceContext* serviceContext, FTDCController* controller) {
    registerServerCollectors(controller);
    registerNetworkingCollectors(controller);

    if (auto rc = getGlobalRC(); rc && isRepl(*rc)) {
        // CmdReplSetGetStatus
        controller->addPeriodicCollector(std::make_unique<FTDCSimpleInternalCommandCollector>(
            "replSetGetStatus", "replSetGetStatus", DatabaseName::kEmpty, replSetGetStatusObj));


        // CollectionStats
        const std::vector<CollectionStatsSpec> specs = getCollectionSpecs(serviceContext);

        for (const auto& spec : specs) {
            controller->addPeriodicCollector(
                makeFilteredCollector(isDataStoringNode,
                                      std::make_unique<FTDCSimpleInternalCommandCollector>(
                                          "aggregate",
                                          spec.statsName,
                                          spec.dbName,
                                          BSONObjBuilder{}
                                              .append("aggregate", spec.collName)
                                              .append("cursor", BSONObj{})
                                              .append("pipeline", pipelineObj)
                                              .obj())));
        }
    }

    controller->addPeriodicMetadataCollector(std::make_unique<FTDCSimpleInternalCommandCollector>(
        "getParameter", "getParameter", DatabaseName::kEmpty, getParameterQueryObj));

    controller->addPeriodicMetadataCollector(
        std::make_unique<FTDCSimpleInternalCommandCollector>("getClusterParameter",
                                                             "getClusterParameter",
                                                             DatabaseName::kEmpty,
                                                             getClusterParameterQueryObj));

    controller->addPeriodicCollector(
        makeFilteredCollector(isDataStoringNode, std::make_unique<FTDCCollectionStatsCollector>()));
}

}  // namespace

void startMongoDFTDC(ServiceContext* serviceContext) {
    auto dir = getFTDCDirectoryPathParameter();

    if (dir.empty()) {
        dir = storageGlobalParams.dbpath;
        dir /= std::string{kFTDCDefaultDirectory};
    }

    std::vector<RegisterCollectorsFunction> registerFns{
        registerShardCollectors,
    };

    startFTDC(serviceContext, dir, FTDCStartMode::kStart, std::move(registerFns));
}

void stopMongoDFTDC() {
    stopFTDC();
}

}  // namespace mongo
