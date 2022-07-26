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
#include "mongo/db/db_raii.h"
#include "mongo/db/free_mon/free_mon_controller.h"
#include "mongo/db/free_mon/free_mon_message.h"
#include "mongo/db/free_mon/free_mon_mongod_gen.h"
#include "mongo/db/free_mon/free_mon_network.h"
#include "mongo/db/free_mon/free_mon_op_observer.h"
#include "mongo/db/free_mon/free_mon_options.h"
#include "mongo/db/free_mon/free_mon_protocol_gen.h"
#include "mongo/db/free_mon/free_mon_storage.h"
#include "mongo/db/ftdc/ftdc_server.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/rpc/object_check.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/future.h"
#include "mongo/util/net/http_client.h"
#include "mongo/util/testing_proctor.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl


namespace mongo {

namespace {

constexpr Seconds kDefaultMetricsGatherInterval(60);

auto makeTaskExecutor(ServiceContext* /*serviceContext*/) {
    ThreadPool::Options tpOptions;
    tpOptions.poolName = "FreeMonHTTP";
    tpOptions.maxThreads = 2;
    tpOptions.onCreateThread = [](const std::string& threadName) {
        Client::initThread(threadName.c_str());
    };
    return std::make_unique<executor::ThreadPoolTaskExecutor>(
        std::make_unique<ThreadPool>(tpOptions), executor::makeNetworkInterface("FreeMonNet"));
}

class FreeMonNetworkHttp final : public FreeMonNetworkInterface {
public:
    explicit FreeMonNetworkHttp(ServiceContext* serviceContext) {
        _executor = makeTaskExecutor(serviceContext);
        _executor->startup();
        _client = HttpClient::create();
        _client->allowInsecureHTTP(TestingProctor::instance().isEnabled());
        _client->setHeaders({"Content-Type: application/octet-stream",
                             "Accept: application/octet-stream",
                             "Expect:"});
    }

    Future<FreeMonRegistrationResponse> sendRegistrationAsync(
        const FreeMonRegistrationRequest& req) override {
        BSONObj reqObj = req.toBSON();
        auto data = std::make_shared<std::vector<std::uint8_t>>(
            reqObj.objdata(), reqObj.objdata() + reqObj.objsize());

        return post("/register", data).then([](DataBuilder&& blob) {
            if (!blob.size()) {
                uasserted(ErrorCodes::FreeMonHttpTemporaryFailure, "Empty response received");
            }

            auto blobSize = blob.size();
            auto blobData = blob.release();
            ConstDataRange cdr(blobData.get(), blobSize);
            BSONObj respObj = cdr.read<Validated<BSONObj>>();

            auto resp = FreeMonRegistrationResponse::parse(IDLParserContext("response"), respObj);

            return resp;
        });
    }

    Future<FreeMonMetricsResponse> sendMetricsAsync(const FreeMonMetricsRequest& req) override {
        BSONObj reqObj = req.toBSON();
        auto data = std::make_shared<std::vector<std::uint8_t>>(
            reqObj.objdata(), reqObj.objdata() + reqObj.objsize());

        return post("/metrics", data).then([](DataBuilder&& blob) {
            if (!blob.size()) {
                uasserted(ErrorCodes::FreeMonHttpTemporaryFailure, "Empty response received");
            }

            auto blobSize = blob.size();
            auto blobData = blob.release();
            ConstDataRange cdr(blobData.get(), blobSize);

            BSONObj respObj = cdr.read<Validated<BSONObj>>();

            auto resp = FreeMonMetricsResponse::parse(IDLParserContext("response"), respObj);

            return resp;
        });
    }

private:
    Future<DataBuilder> post(StringData path,
                             std::shared_ptr<std::vector<std::uint8_t>> data) const {
        auto pf = makePromiseFuture<DataBuilder>();
        std::string url(FreeMonEndpointURL + path.toString());

        auto status = _executor->scheduleWork(
            [promise = std::move(pf.promise), url = std::move(url), data = std::move(data), this](
                const executor::TaskExecutor::CallbackArgs& cbArgs) mutable {
                ConstDataRange cdr(data->data(), data->size());
                try {
                    auto result = this->_client->post(url, cdr);
                    promise.emplaceValue(std::move(result));
                } catch (...) {
                    promise.setError(exceptionToStatus());
                }
            });

        uassertStatusOK(status);
        return std::move(pf.future);
    }

private:
    std::unique_ptr<HttpClient> _client;
    std::unique_ptr<executor::ThreadPoolTaskExecutor> _executor;
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
                                  << "opLatencies" << false << "opcountersRepl" << false
                                  << "opcounters" << false << "transactions" << false
                                  << "connections" << false << "network" << false << "tcMalloc"
                                  << false << "network" << false << "wiredTiger" << false
                                  << "sharding" << false << "metrics" << false)) {}

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
        auto catalog = CollectionCatalog::get(opCtx);
        for (auto& nss : _namespaces) {
            auto optUUID = catalog->lookupUUIDByNSS(opCtx, nss);
            if (optUUID) {
                builder << nss.toString() << optUUID.get();
            }
        }
    }

private:
    std::set<NamespaceString> _namespaces;
};

}  // namespace

Status onValidateFreeMonEndpointURL(StringData str) {
    // Check for http, not https here because testEnabled may not be set yet
    if (!str.startsWith("http"_sd) != 0) {
        return Status(ErrorCodes::BadValue,
                      "cloudFreeMonitoringEndpointURL only supports http:// URLs");
    }

    return Status::OK();
}

void registerCollectors(FreeMonController* controller) {
    // These are collected only at registration
    //
    // CmdBuildInfo
    controller->addRegistrationCollector(std::make_unique<FTDCSimpleInternalCommandCollector>(
        "buildInfo", "buildInfo", "", BSON("buildInfo" << 1)));

    // HostInfoCmd
    controller->addRegistrationCollector(std::make_unique<FTDCSimpleInternalCommandCollector>(
        "hostInfo", "hostInfo", "", BSON("hostInfo" << 1)));

    // Add storageEngine section from serverStatus
    controller->addRegistrationCollector(
        std::make_unique<FreeMonLocalStorageEngineStatusCollector>());

    // Gather one document from local.clustermanager
    controller->addRegistrationCollector(std::make_unique<FreeMonLocalClusterManagerCollector>());

    // These are periodically for metrics upload
    //
    controller->addMetricsCollector(std::make_unique<FTDCSimpleInternalCommandCollector>(
        "getDiagnosticData", "diagnosticData", "", BSON("getDiagnosticData" << 1)));

    // These are collected at registration and as metrics periodically
    //
    if (repl::ReplicationCoordinator::get(getGlobalServiceContext())->getReplicationMode() !=
        repl::ReplicationCoordinator::modeNone) {
        // CmdReplSetGetConfig
        controller->addRegistrationCollector(std::make_unique<FTDCSimpleInternalCommandCollector>(
            "replSetGetConfig", "replSetGetConfig", "", BSON("replSetGetConfig" << 1)));

        controller->addMetricsCollector(std::make_unique<FTDCSimpleInternalCommandCollector>(
            "replSetGetConfig", "replSetGetConfig", "", BSON("replSetGetConfig" << 1)));

        // Collect UUID for certain collections.
        std::set<NamespaceString> namespaces({NamespaceString("local.oplog.rs")});
        controller->addRegistrationCollector(
            std::make_unique<FreeMonNamespaceUUIDCollector>(namespaces));
        controller->addMetricsCollector(
            std::make_unique<FreeMonNamespaceUUIDCollector>(namespaces));
    }

    controller->addRegistrationCollector(std::make_unique<FTDCSimpleInternalCommandCollector>(
        "isMaster", "isMaster", "", BSON("isMaster" << 1)));

    controller->addMetricsCollector(std::make_unique<FTDCSimpleInternalCommandCollector>(
        "isMaster", "isMaster", "", BSON("isMaster" << 1)));
}

void startFreeMonitoring(ServiceContext* serviceContext) {
    if (globalFreeMonParams.freeMonitoringState == EnableCloudStateEnum::kOff) {
        return;
    }

    if (!TestingProctor::instance().isEnabled()) {
        uassert(50774,
                "ExportedFreeMonEndpointURL only supports https:// URLs",
                FreeMonEndpointURL.compare(0, 5, "https") == 0);
    }

    auto network = std::unique_ptr<FreeMonNetworkInterface>(new FreeMonNetworkHttp(serviceContext));

    auto controller = std::make_unique<FreeMonController>(std::move(network));

    auto controllerPtr = controller.get();

    registerCollectors(controller.get());

    // Install the new controller
    FreeMonController::init(getGlobalServiceContext(), std::move(controller));

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

    controllerPtr->start(registrationType,
                         globalFreeMonParams.freeMonitoringTags,
                         Seconds(kDefaultMetricsGatherInterval));
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
    auto controller = FreeMonController::get(getGlobalServiceContext());

    if (controller != nullptr) {
        controller->notifyOnTransitionToPrimary();
    }
}

void setupFreeMonitoringOpObserver(OpObserverRegistry* registry) {
    registry->addObserver(std::make_unique<FreeMonOpObserver>());
}

}  // namespace mongo
