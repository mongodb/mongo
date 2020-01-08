/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/client.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/util/net/ocsp/ocsp_manager.h"

namespace mongo {

namespace {

auto makeTaskExecutor() {
    ThreadPool::Options tpOptions;
    tpOptions.poolName = "OCSPManagerHTTP";
    tpOptions.maxThreads = 10;
    tpOptions.onCreateThread = [](const std::string& threadName) {
        Client::initThread(threadName.c_str());
    };
    return std::make_unique<ThreadPool>(tpOptions);
}

}  // namespace

OCSPManager::OCSPManager() {
    _pool = makeTaskExecutor();

    _client = HttpClient::create();
    if (!_client) {
        return;
    }

    _client->allowInsecureHTTP(true);
    _client->setTimeout(kOCSPRequestTimeoutSeconds);
    _client->setHeaders({"Content-Type: application/ocsp-request"});
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
                                                        StringData responderURI) {
    if (!this->_client) {
        return Future<std::vector<uint8_t>>::makeReady(
            Status(ErrorCodes::InternalErrorNotSupported, "HTTP Client not supported"));
    }

    auto pf = makePromiseFuture<DataBuilder>();
    std::string uri("http://" + responderURI);

    _pool->schedule(
        [this, promise = std::move(pf.promise), uri = std::move(uri), data = std::move(data)](
            auto status) mutable {
            if (!status.isOK()) {
                return;
            }
            try {
                auto result = this->_client->post(uri, data);
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