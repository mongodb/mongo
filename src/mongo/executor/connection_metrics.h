/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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
