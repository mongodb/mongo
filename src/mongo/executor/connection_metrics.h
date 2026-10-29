// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/clock_source.h"
#include "mongo/util/duration.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"

#include <boost/optional.hpp>

namespace mongo {

/**
 * A class to record the metrics for various phases of connection establishment.
 *
 * This class expects a clock source to measure the duration of each phase of the connection
 * establishment. The clock source must always outlive instances of this class.
 *
 * The methods that record the individual phases must be called in the following order:
 * - onConnectionStarted()
 * - onDNSResolved()
 * - onTCPConnectionEstablished()
 * - onTLSHandshakeFinished()
 * - onAuthFinished()
 * - onConnectionHookFinished()
 * The calls for TLS handshake and post-auth hook can be skipped. All other calls are mandatory.
 */
class ConnectionMetrics {
public:
    ConnectionMetrics(ClockSource* clkSource) : _clkSource(clkSource) {}

    const boost::optional<Milliseconds>& dnsResolution() const {
        return _dnsResolution;
    }
    const boost::optional<Milliseconds>& tcpConnection() const {
        return _tcpConnection;
    }
    const boost::optional<Milliseconds>& tlsHandshake() const {
        return _tlsHandshake;
    }
    const boost::optional<Milliseconds>& auth() const {
        return _auth;
    }
    const boost::optional<Milliseconds>& connectionHook() const {
        return _connectionHook;
    }
    Milliseconds total() const {
        return _total;
    }

    void onConnectionStarted() {
        invariant(!_stopWatch);
        _stopWatch.emplace(_clkSource->makeStopWatch());
    }

    void onDNSResolved() {
        invariant(_stopWatch);
        invariant(!_dnsResolution);
        _dnsResolution = _finishPhase();
    }

    void onTCPConnectionEstablished() {
        invariant(_dnsResolution);
        invariant(!_tcpConnection);
        _tcpConnection = _finishPhase();
    }

    void onTLSHandshakeFinished() {
        invariant(_tcpConnection);
        invariant(!_tlsHandshake);
        invariant(!_auth);
        _tlsHandshake = _finishPhase();
    }

    void onAuthFinished() {
        invariant(_tcpConnection);
        invariant(!_auth);
        _auth = _finishPhase();
    }

    void onConnectionHookFinished() {
        invariant(_auth);
        invariant(!_connectionHook);
        _connectionHook = _finishPhase();
    }

    void startConnAcquiredTimer() {
        _fromConnAcquiredTimer.reset();
    }

    Timer* getConnAcquiredTimer() {
        return &_fromConnAcquiredTimer;
    }

private:
    Milliseconds _finishPhase() {
        auto elapsed = _stopWatch->elapsed();
        _stopWatch->restart();
        _total += elapsed;
        return elapsed;
    }

    ClockSource* const _clkSource;
    boost::optional<ClockSource::StopWatch> _stopWatch;
    boost::optional<Milliseconds> _dnsResolution;
    boost::optional<Milliseconds> _tcpConnection;
    boost::optional<Milliseconds> _tlsHandshake;
    boost::optional<Milliseconds> _auth;
    boost::optional<Milliseconds> _connectionHook;
    // A timer that is initialized from when an egress connection is acquired from the connection
    // pool.
    Timer _fromConnAcquiredTimer;
    Milliseconds _total{0};
};

}  // namespace mongo
