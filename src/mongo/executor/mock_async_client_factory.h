// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/async_client_factory.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"
#include "mongo/util/net/hostandport.h"

#include <cstddef>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace mongo::executor {

/**
 * An AsyncClientFactory that never produces clients. Every get()/lease() call adds a Request with
 * the arguments NetworkInterfaceTL passed and returns a future the test fails with Request::fail().
 * Like the real ConnectionPool, a pending request is failed with CallbackCanceled if its
 * CancellationToken is cancelled first.
 */
class MockAsyncClientFactory : public AsyncClientFactory {
public:
    class Request {
    public:
        HostAndPort target;
        transport::ConnectSSLMode sslMode;
        Milliseconds timeout;
        CancellationToken token;
        bool isLease;

        /** True once fail() or token cancellation has settled this request. */
        bool settled() const {
            std::lock_guard lk(_state->mutex);
            return _state->settled;
        }

        /** Fails the pending future with `status`. Does nothing if already settled. */
        void fail(Status status) {
            invariant(!status.isOK());
            std::lock_guard lk(_state->mutex);
            if (std::exchange(_state->settled, true)) {
                return;
            }
            _state->promise.setError(std::move(status));
        }

    private:
        friend class MockAsyncClientFactory;

        struct State {
            std::mutex mutex;
            bool settled = false;
            Promise<std::shared_ptr<AsyncClientHandle>> promise;
        };

        Request(HostAndPort target_,
                transport::ConnectSSLMode sslMode_,
                Milliseconds timeout_,
                CancellationToken token_,
                bool isLease_,
                std::shared_ptr<State> state)
            : target(std::move(target_)),
              sslMode(sslMode_),
              timeout(timeout_),
              token(std::move(token_)),
              isLease(isLease_),
              _state(std::move(state)) {}

        std::shared_ptr<State> _state;
    };

    transport::TransportProtocol getTransportProtocol() const override {
        return transport::TransportProtocol::MongoRPC;
    }

    void startup(ServiceContext* svcCtx,
                 transport::TransportLayer* tl,
                 transport::ReactorHandle reactor) override {}

    /** Fails every still-pending request with ShutdownInProgress. */
    void shutdown() override {
        for (auto&& req : _snapshot()) {
            req.fail(Status(ErrorCodes::ShutdownInProgress, "MockAsyncClientFactory shut down"));
        }
    }

    SemiFuture<std::shared_ptr<AsyncClientHandle>> get(
        const HostAndPort& target,
        transport::ConnectSSLMode sslMode,
        Milliseconds timeout,
        const CancellationToken& token = CancellationToken::uncancelable(),
        ConnectionAcquisitionPurpose purpose = ConnectionAcquisitionPurpose::kNormal) override {
        return _addRequest(target, sslMode, timeout, token, /*isLease=*/false);
    }

    SemiFuture<std::shared_ptr<AsyncClientHandle>> lease(
        const HostAndPort& target,
        transport::ConnectSSLMode sslMode,
        Milliseconds timeout,
        const CancellationToken& token = CancellationToken::uncancelable()) override {
        return _addRequest(target, sslMode, timeout, token, /*isLease=*/true);
    }

    void appendStats(BSONObjBuilder& bob) const override {}
    void dropConnections(const Status& status) override {}
    void dropConnections(const HostAndPort& target, const Status& status) override {}
    void setKeepOpen(const HostAndPort& target, bool keepOpen) override {}

    // Test controls.

    /** Number of get()/lease() calls made so far. */
    size_t requestCount() const {
        std::lock_guard lk(_mutex);
        return _requests.size();
    }

    /** The i-th request in call order. Copies share settlement state with the stored one. */
    Request request(size_t i) const {
        std::lock_guard lk(_mutex);
        invariant(i < _requests.size());
        return _requests[i];
    }

private:
    /** Stores a pending Request for the test and returns its future. */
    SemiFuture<std::shared_ptr<AsyncClientHandle>> _addRequest(const HostAndPort& target,
                                                               transport::ConnectSSLMode sslMode,
                                                               Milliseconds timeout,
                                                               const CancellationToken& token,
                                                               bool isLease) {
        auto state = std::make_shared<Request::State>();
        auto [promise, future] = makePromiseFuture<std::shared_ptr<AsyncClientHandle>>();
        state->promise = std::move(promise);

        Request request(target, sslMode, timeout, token, isLease, state);
        {
            std::lock_guard lk(_mutex);
            _requests.push_back(request);
        }

        // Mirror ConnectionPool: cancellation of the token fails a still-pending request.
        token.onCancel().unsafeToInlineFuture().getAsync([request](Status s) mutable {
            if (s.isOK()) {
                request.fail(Status(ErrorCodes::CallbackCanceled,
                                    "MockAsyncClientFactory request was cancelled"));
            }
        });

        return std::move(future).semi();
    }

    std::vector<Request> _snapshot() const {
        std::lock_guard lk(_mutex);
        return _requests;
    }

    mutable std::mutex _mutex;
    std::vector<Request> _requests;
};

}  // namespace mongo::executor
