// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/status.h"
#include "mongo/util/net/http_client.h"

namespace mongo {

namespace {

class HttpClientProviderImpl : public HttpClientProvider {
public:
    HttpClientProviderImpl() {
        registerHTTPClientProvider(this);
    }

    std::unique_ptr<HttpClient> create() final {
        return nullptr;
    }

    std::unique_ptr<HttpClient> createWithoutConnectionPool() final {
        return nullptr;
    }


    std::unique_ptr<HttpClient> createWithFirewall(const std::vector<CIDR>& cidrDenyList) final {
        return nullptr;
    }

    /**
     * Content for ServerStatus http_client section.
     */
    BSONObj getServerStatus() final {
        return BSONObj();
    }
} provider;

}  // namespace
}  // namespace mongo
