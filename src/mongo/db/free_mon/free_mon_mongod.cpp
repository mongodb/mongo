/**
 * Copyright (C) 2018 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kControl

#include "mongo/platform/basic.h"

#include "mongo/db/free_mon/free_mon_mongod.h"

#include <mutex>
#include <snappy.h>
#include <string>

#include "mongo/base/data_type_validated.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/free_mon/free_mon_controller.h"
#include "mongo/db/free_mon/free_mon_http.h"
#include "mongo/db/free_mon/free_mon_message.h"
#include "mongo/db/free_mon/free_mon_network.h"
#include "mongo/db/free_mon/free_mon_op_observer.h"
#include "mongo/db/free_mon/free_mon_options.h"
#include "mongo/db/free_mon/free_mon_protocol_gen.h"
#include "mongo/db/free_mon/free_mon_storage.h"
#include "mongo/db/ftdc/ftdc_server.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/rpc/object_check.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/future.h"
#include "mongo/util/log.h"

namespace mongo {

namespace {
/**
 * Expose cloudFreeMonitoringEndpointURL set parameter to URL for free monitoring.
 */
class ExportedFreeMonEndpointURL : public LockedServerParameter<std::string> {
public:
    ExportedFreeMonEndpointURL()
        : LockedServerParameter<std::string>("cloudFreeMonitoringEndpointURL",
                                             "https://cloud.mongodb.com/freemonitoring/mongo",
                                             ServerParameterType::kStartupOnly) {}


    Status setFromString(const std::string& str) final {
        // Check for http, not https here because testEnabled may not be set yet
        if (str.compare(0, 4, "http") != 0) {
            return Status(ErrorCodes::BadValue,
                          "ExportedFreeMonEndpointURL only supports https:// URLs");
        }

        return setLocked(str);
    }
} exportedExportedFreeMonEndpointURL;


class FreeMonNetworkHttp : public FreeMonNetworkInterface {
public:
    explicit FreeMonNetworkHttp(std::unique_ptr<FreeMonHttpClientInterface> client)
        : _client(std::move(client)) {}
    ~FreeMonNetworkHttp() final = default;

    Future<FreeMonRegistrationResponse> sendRegistrationAsync(
        const FreeMonRegistrationRequest& req) override {
        BSONObj reqObj = req.toBSON();

        return _client
            ->postAsync(exportedExportedFreeMonEndpointURL.getLocked() + "/register", reqObj)
            .then([](std::vector<uint8_t> blob) {

                if (blob.empty()) {
                    uasserted(ErrorCodes::FreeMonHttpTemporaryFailure, "Empty response received");
                }

                ConstDataRange cdr(reinterpret_cast<char*>(blob.data()), blob.size());

                auto swDoc = cdr.read<Validated<BSONObj>>();
                uassertStatusOK(swDoc.getStatus());

                BSONObj respObj(swDoc.getValue());

                auto resp =
                    FreeMonRegistrationResponse::parse(IDLParserErrorContext("response"), respObj);

                return resp;
            });
    }

    Future<FreeMonMetricsResponse> sendMetricsAsync(const FreeMonMetricsRequest& req) override {
        BSONObj reqObj = req.toBSON();

        return _client
            ->postAsync(exportedExportedFreeMonEndpointURL.getLocked() + "/metrics", reqObj)
            .then([](std::vector<uint8_t> blob) {

                if (blob.empty()) {
                    uasserted(ErrorCodes::FreeMonHttpTemporaryFailure, "Empty response received");
                }

                ConstDataRange cdr(reinterpret_cast<char*>(blob.data()), blob.size());

                auto swDoc = cdr.read<Validated<BSONObj>>();
                uassertStatusOK(swDoc.getStatus());

                BSONObj respObj(swDoc.getValue());

                auto resp =
                    FreeMonMetricsResponse::parse(IDLParserErrorContext("response"), respObj);

                return resp;
            });
    }

private:
    std::unique_ptr<FreeMonHttpClientInterface> _client;
};

/**
 * Collect the mms-automation state document from local.clustermanager during registration.
 */
class FreeMonLocalClusterManagerCollector : public FreeMonCollectorInterface {
public:
    std::string name() const final {
        return "clustermanager";
    }

    void collect(OperationContext* opCtx, BSONObjBuilder& builder) {
        auto optionalObj = FreeMonStorage::readClusterManagerState(opCtx);
        if (optionalObj.is_initialized()) {
            builder.appendElements(optionalObj.get());
        }
    }
};

/**
 * Get the "storageEngine" section of "serverStatus" during registration.
 */
class FreeMonLocalStorageEngineStatusCollector : public FTDCSimpleInternalCommandCollector {
public:
    FreeMonLocalStorageEngineStatusCollector()
        : FTDCSimpleInternalCommandCollector(
              "serverStatus",
              "serverStatus",
              "",
              // Try to filter server status to make it cheaper to collect. Harmless if we gather
              // extra
              BSON("serverStatus" << 1 << "storageEngine" << true << "extra_info" << false
                                  << "opLatencies"
                                  << false
                                  << "opcountersRepl"
                                  << false
                                  << "opcounters"
                                  << false
                                  << "transactions"
                                  << false
                                  << "connections"
                                  << false
                                  << "network"
                                  << false
                                  << "tcMalloc"
                                  << false
                                  << "network"
                                  << false
                                  << "wiredTiger"
                                  << false
                                  << "sharding"
                                  << false
                                  << "metrics"
                                  << false)) {}

    std::string name() const final {
        return "storageEngine";
    }

    void collect(OperationContext* opCtx, BSONObjBuilder& builder) {
        BSONObjBuilder localBuilder;

        FTDCSimpleInternalCommandCollector::collect(opCtx, localBuilder);

        BSONObj obj = localBuilder.obj();

        builder.appendElements(obj["storageEngine"].Obj());
    }
};

/**
 * Collect the UUIDs associated with the named collections (if available).
 */
class FreeMonNamespaceUUIDCollector : public FreeMonCollectorInterface {
public:
    FreeMonNamespaceUUIDCollector(std::set<NamespaceString> namespaces)
        : _namespaces(std::move(namespaces)) {}

    std::string name() const final {
        return "uuid";
    }

    void collect(OperationContext* opCtx, BSONObjBuilder& builder) {
        for (auto nss : _namespaces) {
            AutoGetCollectionForRead coll(opCtx, nss);
            auto* collection = coll.getCollection();
            if (collection) {
                auto optUUID = collection->uuid();
                if (optUUID) {
                    builder << nss.toString() << optUUID.get();
                }
            }
        }
    }

private:
    std::set<NamespaceString> _namespaces;
};

}  // namespace


auto makeTaskExecutor(ServiceContext* /*serviceContext*/) {
    ThreadPool::Options tpOptions;
    tpOptions.poolName = "freemon";
    tpOptions.maxThreads = 2;
    tpOptions.onCreateThread = [](const std::string& threadName) {
        Client::initThread(threadName.c_str());
    };
    return stdx::make_unique<executor::ThreadPoolTaskExecutor>(
        std::make_unique<ThreadPool>(tpOptions), executor::makeNetworkInterface("FreeMon"));
}

void registerCollectors(FreeMonController* controller) {
    // These are collected only at registration
    //
    // CmdBuildInfo
    controller->addRegistrationCollector(stdx::make_unique<FTDCSimpleInternalCommandCollector>(
        "buildInfo", "buildInfo", "", BSON("buildInfo" << 1)));

    // HostInfoCmd
    controller->addRegistrationCollector(stdx::make_unique<FTDCSimpleInternalCommandCollector>(
        "hostInfo", "hostInfo", "", BSON("hostInfo" << 1)));

    // Add storageEngine section from serverStatus
    controller->addRegistrationCollector(
        stdx::make_unique<FreeMonLocalStorageEngineStatusCollector>());

    // Gather one document from local.clustermanager
    controller->addRegistrationCollector(stdx::make_unique<FreeMonLocalClusterManagerCollector>());

    // These are periodically for metrics upload
    //
    controller->addMetricsCollector(stdx::make_unique<FTDCSimpleInternalCommandCollector>(
        "getDiagnosticData", "diagnosticData", "", BSON("getDiagnosticData" << 1)));

    // These are collected at registration and as metrics periodically
    //
    if (repl::ReplicationCoordinator::get(getGlobalServiceContext())->getReplicationMode() !=
        repl::ReplicationCoordinator::modeNone) {
        // CmdReplSetGetConfig
        controller->addRegistrationCollector(stdx::make_unique<FTDCSimpleInternalCommandCollector>(
            "replSetGetConfig", "replSetGetConfig", "", BSON("replSetGetConfig" << 1)));

        controller->addMetricsCollector(stdx::make_unique<FTDCSimpleInternalCommandCollector>(
            "replSetGetConfig", "replSetGetConfig", "", BSON("replSetGetConfig" << 1)));

        // Collect UUID for certain collections.
        std::set<NamespaceString> namespaces({NamespaceString("local.oplog.rs")});
        controller->addRegistrationCollector(
            std::make_unique<FreeMonNamespaceUUIDCollector>(namespaces));
        controller->addMetricsCollector(
            std::make_unique<FreeMonNamespaceUUIDCollector>(namespaces));
    }

    controller->addRegistrationCollector(stdx::make_unique<FTDCSimpleInternalCommandCollector>(
        "isMaster", "isMaster", "", BSON("isMaster" << 1)));

    controller->addMetricsCollector(stdx::make_unique<FTDCSimpleInternalCommandCollector>(
        "isMaster", "isMaster", "", BSON("isMaster" << 1)));
}

void startFreeMonitoring(ServiceContext* serviceContext) {
    if (globalFreeMonParams.freeMonitoringState == EnableCloudStateEnum::kOff) {
        return;
    }

    // Check for http, not https here because testEnabled may not be set yet
    if (!getTestCommandsEnabled()) {
        uassert(50774,
                "ExportedFreeMonEndpointURL only supports https:// URLs",
                exportedExportedFreeMonEndpointURL.getLocked().compare(0, 5, "https") == 0);
    }

    auto executor = makeTaskExecutor(serviceContext);

    executor->startup();

    auto http = createFreeMonHttpClient(std::move(executor));
    if (http == nullptr) {
        // HTTP init failed
        return;
    }

    auto network =
        std::unique_ptr<FreeMonNetworkInterface>(new FreeMonNetworkHttp(std::move(http)));

    auto controller = stdx::make_unique<FreeMonController>(std::move(network));

    auto controllerPtr = controller.get();

    registerCollectors(controller.get());

    // Install the new controller
    FreeMonController::set(getGlobalServiceContext(), std::move(controller));

    RegistrationType registrationType = RegistrationType::DoNotRegister;
    if (globalFreeMonParams.freeMonitoringState == EnableCloudStateEnum::kOn) {
        // If replication is enabled, we may need to register on becoming primary
        if (repl::ReplicationCoordinator::get(getGlobalServiceContext())->getReplicationMode() !=
            repl::ReplicationCoordinator::modeNone) {
            registrationType = RegistrationType::RegisterAfterOnTransitionToPrimary;
        } else {
            registrationType = RegistrationType::RegisterOnStart;
        }
    } else if (globalFreeMonParams.freeMonitoringState == EnableCloudStateEnum::kRuntime) {
        registrationType = RegistrationType::RegisterAfterOnTransitionToPrimaryIfEnabled;
    }

    controllerPtr->start(registrationType, globalFreeMonParams.freeMonitoringTags);
}

void stopFreeMonitoring() {
    if (globalFreeMonParams.freeMonitoringState == EnableCloudStateEnum::kOff) {
        return;
    }

    auto controller = FreeMonController::get(getGlobalServiceContext());

    if (controller != nullptr) {
        controller->stop();
    }
}

void notifyFreeMonitoringOnTransitionToPrimary() {
    FreeMonController::get(getGlobalServiceContext())->notifyOnTransitionToPrimary();
}

void setupFreeMonitoringOpObserver(OpObserverRegistry* registry) {
    registry->addObserver(stdx::make_unique<FreeMonOpObserver>());
}

FreeMonHttpClientInterface::~FreeMonHttpClientInterface() = default;

}  // namespace mongo
