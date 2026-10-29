// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/util/net/ocsp/ocsp_manager.h"

#include "mongo/db/client.h"
#include "mongo/util/net/ssl_parameters_gen.h"

#include <memory>
#include <string_view>

namespace mongo {

namespace {

const auto getOCSPManager = ServiceContext::declareDecoration<std::unique_ptr<OCSPManager>>();

auto makeTaskExecutor() {
    // This task executor's threads are technically killable. However, since we never create an
    // operation context in any of the tasks which we run on this executor, we don't have to worry
    // about handling interrupts.
    return ThreadPool::make({
        .poolName = "OCSPManagerHTTP",
        .maxThreads = 10,
        .onCreateThread =
            [](const std::string& threadName) {
                Client::initThread(threadName, getGlobalServiceContext()->getService());
            },
    });
}

}  // namespace

OCSPManager* OCSPManager::get(ServiceContext* service) {
    return getOCSPManager(service).get();
}

void OCSPManager::start(ServiceContext* service) {
    auto manager = std::make_unique<OCSPManager>();

    manager->startThreadPool();

    getOCSPManager(service) = std::move(manager);
}

void OCSPManager::shutdown(ServiceContext* service) {
    auto* ocspManager = get(service);
    if (ocspManager) {
        ocspManager->_pool->shutdown();
        ocspManager->_pool->join();
    }
}

OCSPManager::OCSPManager() {
    _pool = makeTaskExecutor();

    this->_tlsServerHttp = HttpClient::create();
    this->_tlsClientHttp = HttpClient::create();

    if (!this->_tlsServerHttp) {
        return;
    }

    this->_tlsClientHttp->allowInsecureHTTP(true);
    this->_tlsClientHttp->setTimeout(Seconds(gTLSOCSPVerifyTimeoutSecs));
    this->_tlsClientHttp->setHeaders({"Content-Type: application/ocsp-request"});

    this->_tlsServerHttp->allowInsecureHTTP(true);
    if (gTLSOCSPStaplingTimeoutSecs < 0) {
        this->_tlsServerHttp->setTimeout(Seconds(gTLSOCSPVerifyTimeoutSecs));
    } else {
        this->_tlsServerHttp->setTimeout(Seconds(gTLSOCSPStaplingTimeoutSecs));
    }
    this->_tlsServerHttp->setHeaders({"Content-Type: application/ocsp-request"});
}

void OCSPManager::startThreadPool() {
    if (_pool) {
        _pool->startup();
    }
}

/**
 * Constructs the HTTP client and sends the OCSP request to the responder.
 * Returns a vector of bytes to be constructed into a OCSP response.
 */
Future<std::vector<uint8_t>> OCSPManager::requestStatus(std::vector<uint8_t> data,
                                                        std::string_view responderURI,
                                                        OCSPPurpose purpose) {
    if (!this->_tlsClientHttp || !this->_tlsServerHttp) {
        return Future<std::vector<uint8_t>>::makeReady(
            Status(ErrorCodes::InternalErrorNotSupported, "HTTP Client not supported"));
    }

    auto pf = makePromiseFuture<DataBuilder>();
    std::string uri("http://" + std::string{responderURI});

    _pool->schedule([this,
                     purpose,
                     promise = std::move(pf.promise),
                     uri = std::move(uri),
                     data = std::move(data)](auto status) mutable {
        if (!status.isOK()) {
            return;
        }
        try {
            const auto& client =
                purpose == OCSPPurpose::kClientVerify ? this->_tlsClientHttp : this->_tlsServerHttp;
            auto result = client->post(uri, data);
            promise.emplaceValue(std::move(result));
        } catch (...) {
            promise.setError(exceptionToStatus());
        }
    });

    return std::move(pf.future).then(
        [](DataBuilder dataBuilder) mutable -> Future<std::vector<uint8_t>> {
            if (dataBuilder.size() == 0) {
                return Status(ErrorCodes::SSLHandshakeFailed, "Failed to acquire OCSP Response.");
            }

            auto blobSize = dataBuilder.size();
            auto blobData = dataBuilder.release();
            return std::vector<uint8_t>(blobData.get(), blobData.get() + blobSize);
        });
}

}  // namespace mongo
