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
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/sdam/sdam.h"
#include "mongo/client/sdam/sdam_datatypes.h"
#include "mongo/executor/network_interface.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/net/hostandport.h"

#include <mutex>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
class StreamableReplicaSetMonitorErrorHandler {
public:
    struct ErrorActions {
        bool dropConnections = false;
        bool requestImmediateCheck = false;
        boost::optional<sdam::HelloOutcome> helloOutcome;
        BSONObj toBSON() const;
    };

    // Identifies which monitoring or application event triggered error action computation.
    enum class TriggerEvent {
        // An application (non-monitoring) operation failed on a connection before the initial
        // hello exchange completed.
        kApplicationPreHandshake,

        // An application (non-monitoring) operation failed on an established connection.
        kApplicationPostHandshake,

        // The background SDAM monitoring heartbeat (periodic hello command) failed on an
        // established connection.
        kHeartbeatFailure,

        // A background monitoring ping failed on an established connection.
        kPingFailure,

        // The hello exchange failed during new connection setup. Fires for any new connection
        // through the RSM's dedicated NetworkInterface via the
        // ReplicaSetMonitorManagerNetworkConnectionHook.
        kHandshakeFailure,
    };

    bool isApplicationEvent(TriggerEvent triggerEvent) const {
        return triggerEvent == TriggerEvent::kApplicationPreHandshake ||
            triggerEvent == TriggerEvent::kApplicationPostHandshake;
    }

    bool isPreHandshakeEvent(TriggerEvent triggerEvent) const {
        return triggerEvent == TriggerEvent::kApplicationPreHandshake ||
            triggerEvent == TriggerEvent::kHandshakeFailure;
    }

    bool isPostHandshakeEvent(TriggerEvent triggerEvent) const {
        return triggerEvent == TriggerEvent::kApplicationPostHandshake ||
            triggerEvent == TriggerEvent::kHeartbeatFailure ||
            triggerEvent == TriggerEvent::kPingFailure;
    }

    virtual ~StreamableReplicaSetMonitorErrorHandler() = default;

    // Based on the error status, source of the error, and trigger event determine what
    // ErrorActions we should take.
    virtual ErrorActions computeErrorActions(const HostAndPort& host,
                                             const Status& status,
                                             TriggerEvent triggerEvent,
                                             BSONObj bson) = 0;

protected:
    sdam::HelloOutcome _createErrorHelloOutcome(const HostAndPort& host,
                                                boost::optional<BSONObj> bson,
                                                const Status& status) const {
        return sdam::HelloOutcome(host, bson ? *bson : BSONObj(), status.toString());
    }
};

class SdamErrorHandler final : public StreamableReplicaSetMonitorErrorHandler {
public:
    explicit SdamErrorHandler(std::string setName) : _setName(std::move(setName)) {};

    ErrorActions computeErrorActions(const HostAndPort& host,
                                     const Status& status,
                                     TriggerEvent triggerEvent,
                                     BSONObj bson) override;

private:
    int _getConsecutiveErrorsWithoutHelloOutcome(const HostAndPort& host) const;
    void _incrementConsecutiveErrorsWithoutHelloOutcome(const HostAndPort& host);
    void _clearConsecutiveErrorsWithoutHelloOutcome(const HostAndPort& host);

    bool _isRetriableError(const Status& status) const;
    bool _isNetworkTimeout(const Status& status) const;
    bool _isNodeShuttingDown(const Status& status) const;
    bool _isNetworkError(const Status& status) const;
    bool _isNotPrimaryError(const Status& status) const;
    bool _isRemoteError(const BSONObj& bson) const;

    const std::string _setName;
    mutable std::mutex _mutex;
    stdx::unordered_map<HostAndPort, int> _consecutiveErrorsWithoutHelloOutcome;
};
}  // namespace mongo
