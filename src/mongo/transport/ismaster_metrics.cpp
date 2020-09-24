/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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
#include "mongo/transport/ismaster_metrics.h"

namespace mongo {
namespace {
const auto HelloMetricsDecoration = ServiceContext::declareDecoration<HelloMetrics>();
const auto InExhaustHelloDecoration = transport::Session::declareDecoration<InExhaustHello>();
}  // namespace

HelloMetrics* HelloMetrics::get(ServiceContext* service) {
    return &HelloMetricsDecoration(service);
}

HelloMetrics* HelloMetrics::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

size_t HelloMetrics::getNumExhaustIsMaster() const {
    return _exhaustIsMasterConnections.load();
}

void HelloMetrics::incrementNumExhaustIsMaster() {
    _exhaustIsMasterConnections.fetchAndAdd(1);
}

void HelloMetrics::decrementNumExhaustIsMaster() {
    _exhaustIsMasterConnections.fetchAndSubtract(1);
}

size_t HelloMetrics::getNumExhaustHello() const {
    return _exhaustHelloConnections.load();
}

void HelloMetrics::incrementNumExhaustHello() {
    _exhaustHelloConnections.fetchAndAdd(1);
}

void HelloMetrics::decrementNumExhaustHello() {
    _exhaustHelloConnections.fetchAndSubtract(1);
}

size_t HelloMetrics::getNumAwaitingTopologyChanges() const {
    return _connectionsAwaitingTopologyChanges.load();
}

void HelloMetrics::incrementNumAwaitingTopologyChanges() {
    _connectionsAwaitingTopologyChanges.fetchAndAdd(1);
}

void HelloMetrics::decrementNumAwaitingTopologyChanges() {
    _connectionsAwaitingTopologyChanges.fetchAndSubtract(1);
}

void HelloMetrics::resetNumAwaitingTopologyChanges() {
    _connectionsAwaitingTopologyChanges.store(0);
}

InExhaustHello* InExhaustHello::get(transport::Session* session) {
    return &InExhaustHelloDecoration(session);
}

InExhaustHello::~InExhaustHello() {
    if (_inExhaustIsMaster) {
        HelloMetrics::get(getGlobalServiceContext())->decrementNumExhaustIsMaster();
    }
    if (_inExhaustHello) {
        HelloMetrics::get(getGlobalServiceContext())->decrementNumExhaustHello();
    }
}

bool InExhaustHello::getInExhaustIsMaster() const {
    return _inExhaustIsMaster;
}

bool InExhaustHello::getInExhaustHello() const {
    return _inExhaustHello;
}

void InExhaustHello::setInExhaust(bool inExhaust, StringData commandName) {
    bool isHello = (commandName == "hello"_sd);

    // Transition out of exhaust hello if setting inExhaust to false or if
    // the isMaster command is used.
    if (_inExhaustHello && (!inExhaust || !isHello)) {
        HelloMetrics::get(getGlobalServiceContext())->decrementNumExhaustHello();
        _inExhaustHello = false;
    }

    // Transition out of exhaust isMaster if setting inExhaust to false or if
    // the hello command is used.
    if (_inExhaustIsMaster && (!inExhaust || isHello)) {
        HelloMetrics::get(getGlobalServiceContext())->decrementNumExhaustIsMaster();
        _inExhaustIsMaster = false;
    }

    if (inExhaust) {
        if (isHello && !_inExhaustHello) {
            HelloMetrics::get(getGlobalServiceContext())->incrementNumExhaustHello();
            _inExhaustHello = inExhaust;
        } else if (!isHello && !_inExhaustIsMaster) {
            HelloMetrics::get(getGlobalServiceContext())->incrementNumExhaustIsMaster();
            _inExhaustIsMaster = inExhaust;
        }
    }
}

}  // namespace mongo
