// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/data_builder.h"
#include "mongo/base/data_range.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/cidr.h"

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace mongo {

constexpr Seconds kConnectionTimeout{60};
constexpr Seconds kTotalRequestTimeout{120};

/**
 * Interface used to upload and receive binary payloads to HTTP servers.
 */
class [[MONGO_MOD_OPEN]] HttpClient {
public:
    enum class HttpMethod { kGET, kPOST, kPUT, kPATCH, kDELETE };

    struct HttpReply {
        std::uint16_t code;
        DataBuilder header;
        DataBuilder body;

        HttpReply(std::uint16_t code_, DataBuilder&& header_, DataBuilder&& body_)
            : code(code_), header(std::move(header_)), body(std::move(body_)) {}
    };

    virtual ~HttpClient() = default;

    /**
     * Configure all future requests on this client to allow insecure http:// urls.
     * By default, only https:// is allowed.
     */
    virtual void allowInsecureHTTP(bool allow) = 0;

    /**
     * Returns Status::OK iff the provided URL endpoint is "secure".
     *
     * HTTPS endpoints are secure. If test commands are enabled, localhost endpoints
     * over HTTP with only a host, optional port, and optionally a slash and trailing
     * content are considered secure.
     */
    static Status endpointIsSecure(std::string_view url);

    /**
     * Assign a set of headers for this request.
     */
    virtual void setHeaders(const std::vector<std::string>& headers) = 0;

    /**
     * Sets the maximum time to wait during the connection phase.
     * `0` indicates no timeout.
     */
    void setConnectTimeout(Seconds timeout) {
        _connectTimeout = timeout;
    }

    /**
     * Sets the maximum time to wait for the total request.
     * `0` indicates no timeout.
     */
    void setTimeout(Seconds timeout) {
        _timeout = timeout;
    }

    /**
     * Perform a request to specified URL.
     * Note that some methods (GET, HEAD) prohibit context bodies.
     */
    virtual HttpReply request(HttpMethod method,
                              std::string_view url,
                              ConstDataRange data = {nullptr, 0}) const = 0;

    /**
     * Convenience wrapper to perform a POST and require a 200 response.
     */
    DataBuilder post(std::string_view url, ConstDataRange data) {
        return requestSuccess(HttpMethod::kPOST, url, data);
    }

    /**
     * Convenience wrapper to perform a PUT and require a 200 response.
     */
    DataBuilder put(std::string_view url, ConstDataRange data) {
        return requestSuccess(HttpMethod::kPUT, url, data);
    }

    /**
     * Convenience wrapper to perform a GET and require a 200 response.
     */
    DataBuilder get(std::string_view url) {
        return requestSuccess(HttpMethod::kGET, url, {nullptr, 0});
    }

    /**
     * Factory method provided by client implementation.
     */
    static std::unique_ptr<HttpClient> create();

    /**
     * Factory method provided by client implementation.
     *
     * The connection pool requires the ability to spawn threads which is not allowed through
     * options parsing. Callers should default to create() unless they are calling into the
     * HttpClient before thread spawning is allowed.
     */
    static std::unique_ptr<HttpClient> createWithoutConnectionPool();

    /**
     * Factory method provided by client implementation.
     *
     * Used by Atlas Stream Processing.
     */
    static std::unique_ptr<HttpClient> createWithFirewall(const std::vector<CIDR>& cidrDenyList);

    /**
     * Content for ServerStatus http_client section.
     */
    static BSONObj getServerStatus();

protected:
    Seconds _timeout = kTotalRequestTimeout;
    Seconds _connectTimeout = kConnectionTimeout;

private:
    DataBuilder requestSuccess(HttpMethod method, std::string_view url, ConstDataRange data) const {
        auto reply = request(method, url, data);
        uassert(ErrorCodes::OperationFailed,
                str::stream() << "Unexpected http status code from server: " << reply.code,
                reply.code == 200);
        return std::move(reply.body);
    }
};

/**
 * HttpClientProvider is the factory behind the HttpClient
 *
 * This exists as a level-of-indirection to break link graph cycles.
 */
class HttpClientProvider {
public:
    virtual ~HttpClientProvider();

    /**
     * Factory method provided by client implementation.
     */
    virtual std::unique_ptr<HttpClient> create() = 0;

    /**
     * Factory method provided by client implementation.
     */
    virtual std::unique_ptr<HttpClient> createWithoutConnectionPool() = 0;

    /**
     * Factory method provided by client implementation.
     */
    virtual std::unique_ptr<HttpClient> createWithFirewall(
        const std::vector<CIDR>& cidrDenyList) = 0;

    /**
     * Content for ServerStatus http_client section.
     */
    virtual BSONObj getServerStatus() = 0;
};

/**
 * Register HTTP Client provider
 */
void registerHTTPClientProvider(HttpClientProvider* factory);

}  // namespace mongo
