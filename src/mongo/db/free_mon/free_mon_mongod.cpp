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

#include "mongo/platform/basic.h"

#include "mongo/db/free_mon/free_mon_mongod.h"

#include <mutex>
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
#include "mongo/db/free_mon/free_mon_http.h"
#include "mongo/db/free_mon/free_mon_message.h"
#include "mongo/db/free_mon/free_mon_network.h"
#include "mongo/db/free_mon/free_mon_protocol_gen.h"
#include "mongo/db/free_mon/free_mon_storage.h"
#include "mongo/db/ftdc/ftdc_server.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/future.h"

namespace mongo {

namespace {

/**
 * Expose cloudFreeMonitoringEndpointURL set parameter to URL for free monitoring.
 */
class ExportedFreeMonEndpointURL : public LockedServerParameter<std::string> {
public:
    ExportedFreeMonEndpointURL()
        : LockedServerParameter<std::string>("cloudFreeMonitoringEndpointURL",
                                             "https://localhost:8080",
                                             ServerParameterType::kStartupOnly) {}


    Status setFromString(const std::string& str) final {
        // Check for http, not https here because testEnabled may not be set yet
        if (!str.compare(0, 4, "http")) {
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

        return _client->postAsync(exportedExportedFreeMonEndpointURL.getURL() + "/register", reqObj)
            .then([](std::vector<uint8_t> blob) {

                if (blob.empty()) {
                    uassertStatusOK(
                        Status(ErrorCodes::FreeMonHttpTemporaryFailure, "Empty response received"));
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

        return _client->postAsync(exportedExportedFreeMonEndpointURL.getURL() + "/metrics", reqObj)
            .then([](std::vector<uint8_t> blob) {

                if (blob.empty()) {
                    uassertStatusOK(
                        Status(ErrorCodes::FreeMonHttpTemporaryFailure, "Empty response received"));
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


auto makeTaskExecutor(ServiceContext* /*serviceContext*/) {
    ThreadPool::Options tpOptions;
    tpOptions.poolName = "freemon";
    tpOptions.maxThreads = 2;
    tpOptions.onCreateThread = [](const std::string& threadName) {
        Client::initThread(threadName.c_str());
    };
    return stdx::make_unique<executor::ThreadPoolTaskExecutor>(
        std::make_unique<ThreadPool>(tpOptions),
        executor::makeNetworkInterface("NetworkInterfaceASIO-FreeMon"));
}

}  // namespace


void startFreeMonitoring(ServiceContext* serviceContext) {

    auto executor = makeTaskExecutor(serviceContext);

    executor->startup();

    auto http = createFreeMonHttpClient(std::move(executor));
    if (http == nullptr) {
        // HTTP init failed
        return;
    }

    auto network =
        std::unique_ptr<FreeMonNetworkInterface>(new FreeMonNetworkHttp(std::move(http)));
}

void stopFreeMonitoring() {}

FreeMonHttpClientInterface::~FreeMonHttpClientInterface() = default;

FreeMonNetworkInterface::~FreeMonNetworkInterface() = default;

}  // namespace mongo
