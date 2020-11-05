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

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/atomic_word.h"

namespace mongo {

/**
 * A decoration on the Session object used to track exhaust metrics. We are
 * tracking metrics for "hello" and "isMaster/ismaster" separately while we
 * support both commands. This allows us insight into which command is being
 * used until we decide to remove support for isMaster completely.
 */
class InExhaustHello {
public:
    InExhaustHello() = default;

    InExhaustHello(const InExhaustHello&) = delete;
    InExhaustHello& operator=(const InExhaustHello&) = delete;
    InExhaustHello(InExhaustHello&&) = delete;
    InExhaustHello& operator=(InExhaustHello&&) = delete;

    static InExhaustHello* get(transport::Session* session);
    void setInExhaust(bool inExhaust, StringData commandName);
    bool getInExhaustIsMaster() const;
    bool getInExhaustHello() const;
    ~InExhaustHello();

private:
    bool _inExhaustIsMaster = false;
    bool _inExhaustHello = false;
};

/**
 * Container for awaitable hello and isMaster statistics. We are tracking
 * metrics for "hello" and "isMaster/ismaster" separately while we support
 * both commands. This allows us insight into which command is being used
 * until we decide to remove support for isMaster completely.
 */
class HelloMetrics {
    HelloMetrics(const HelloMetrics&) = delete;
    HelloMetrics& operator=(const HelloMetrics&) = delete;
    HelloMetrics(HelloMetrics&&) = delete;
    HelloMetrics& operator=(HelloMetrics&&) = delete;

public:
    HelloMetrics() = default;

    static HelloMetrics* get(ServiceContext* service);
    static HelloMetrics* get(OperationContext* opCtx);

    size_t getNumExhaustIsMaster() const;
    size_t getNumExhaustHello() const;

    size_t getNumAwaitingTopologyChanges() const;
    void incrementNumAwaitingTopologyChanges();
    void decrementNumAwaitingTopologyChanges();

    void resetNumAwaitingTopologyChanges();
    friend InExhaustHello;

private:
    void incrementNumExhaustIsMaster();
    void decrementNumExhaustIsMaster();

    void incrementNumExhaustHello();
    void decrementNumExhaustHello();

    // The number of clients currently waiting in isMaster for a topology change.
    AtomicWord<size_t> _connectionsAwaitingTopologyChanges{0};
    // The number of connections whose last request was an isMaster with exhaustAllowed.
    AtomicWord<size_t> _exhaustIsMasterConnections{0};
    // The number of connections whose last request was a hello with exhaustAllowed.
    AtomicWord<size_t> _exhaustHelloConnections{0};
};

}  // namespace mongo
