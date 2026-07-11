// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/transport/session.h"

#include "mongo/base/error_codes.h"
#include "mongo/platform/atomic.h"
#include "mongo/transport/session_manager.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace transport {

MONGO_FAIL_POINT_DEFINE(clientIsConnectedToLoadBalancerPort);
MONGO_FAIL_POINT_DEFINE(clientIsLoadBalancedPeer);

namespace {

Atomic<unsigned long long> sessionIdCounter(0);

}  // namespace

// Note: not initializing _isPreauthIngress to isIngress here because that doesn't correctly handle
// the case where auth is disabled. It is initialized in the SessionWorkflow constructor prior to
// reading the first request.
Session::Session(bool isIngress) : _id(sessionIdCounter.addAndFetch(1)), _isIngress(isIngress) {}

Session::~Session() {
    if (_opCounters && _inOperation) {
        _opCounters->completed.fetchAndAddRelaxed(1);
    }
}

void Session::setInOperation(bool state) {
    if (MONGO_unlikely(!_opCounters)) {
        // We should only take this path once for each connection in production, so we are opting
        // for readability over performance here.
        auto tl = getTransportLayer();
        if (MONGO_unlikely(!tl))
            return;

        auto sm = tl->getSessionManager();
        if (MONGO_unlikely(!sm))
            return;

        _opCounters = sm->getOpCounters();
    }

    auto oldState = std::exchange(_inOperation, state);
    if (state) {
        uassert(ErrorCodes::InvalidOptions,
                "Operation started on session already in an active operation",
                !oldState);
        _opCounters->total.fetchAndAddRelaxed(1);
    } else if (oldState) {
        _opCounters->completed.fetchAndAddRelaxed(1);
    }
}

void Session::setIsLoadBalancerPeer(bool helloHasLoadBalancedOption) {
    tassert(ErrorCodes::BadValue,
            "Client claimed to be from a loadBalancer, but is not on load balancer port",
            isConnectedToLoadBalancerPort() || !helloHasLoadBalancedOption);

    if (_isLoadBalancerPeer == helloHasLoadBalancedOption) {
        return;
    }
    _isLoadBalancerPeer = helloHasLoadBalancedOption;

    if (auto tl = getTransportLayer()) {
        if (auto* sm = tl->getSessionManager()) {
            sm->onLoadBalancerPeerSet(helloHasLoadBalancedOption);
        }
    }
}

}  // namespace transport
}  // namespace mongo
