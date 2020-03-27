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
#include <boost/optional.hpp>

#include "mongo/client/sdam/sdam.h"
#include "mongo/executor/network_interface.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/unordered_map.h"

namespace mongo {
class StreamableReplicaSetMonitorErrorHandler {
public:
    struct ErrorActions {
        bool dropConnections = false;
        bool requestImmediateCheck = false;
        boost::optional<sdam::IsMasterOutcome> isMasterOutcome;
        BSONObj toBSON() const;
    };

    // Indicates whether the initial handshake has completed.
    enum class HandshakeStage { kPreHandshake, kPostHandshake };

    virtual ~StreamableReplicaSetMonitorErrorHandler() = default;

    // Based on the error status, source of the error, and handshake stage determine what
    // ErrorActions we should take.
    virtual ErrorActions computeErrorActions(const HostAndPort& host,
                                             const Status& status,
                                             HandshakeStage handshakeStage,
                                             bool isApplicationOperation,
                                             BSONObj bson) noexcept = 0;

protected:
    sdam::IsMasterOutcome _createErrorIsMasterOutcome(const HostAndPort& host,
                                                      boost::optional<BSONObj> bson,
                                                      const Status& status) const {
        return sdam::IsMasterOutcome(host.toString(), bson ? *bson : BSONObj(), status.toString());
    }
};

class SdamErrorHandler final : public StreamableReplicaSetMonitorErrorHandler {
public:
    explicit SdamErrorHandler(std::string setName) : _setName(std::move(setName)){};

    ErrorActions computeErrorActions(const HostAndPort& host,
                                     const Status& status,
                                     HandshakeStage handshakeStage,
                                     bool isApplicationOperation,
                                     BSONObj bson) noexcept override;

private:
    int _getConsecutiveErrorsWithoutIsMasterOutcome(const HostAndPort& host) const;
    void _incrementConsecutiveErrorsWithoutIsMasterOutcome(const HostAndPort& host);
    void _clearConsecutiveErrorsWithoutIsMasterOutcome(const HostAndPort& host);

    bool _isNodeRecovering(const Status& status) const;
    bool _isNetworkTimeout(const Status& status) const;
    bool _isNodeShuttingDown(const Status& status) const;
    bool _isNetworkError(const Status& status) const;
    bool _isNotMasterOrNotRecovering(const Status& status) const;
    bool _isNotMaster(const Status& status) const;

    const std::string _setName;
    mutable Mutex _mutex;
    stdx::unordered_map<HostAndPort, int> _consecutiveErrorsWithoutIsMasterOutcome;
};
}  // namespace mongo
