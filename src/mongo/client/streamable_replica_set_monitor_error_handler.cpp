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
#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork
#include "mongo/client/streamable_replica_set_monitor_error_handler.h"

#include "mongo/logv2/log.h"

namespace mongo {
SdamErrorHandler::ErrorActions SdamErrorHandler::computeErrorActions(const HostAndPort& host,
                                                                     const Status& status,
                                                                     HandshakeStage handshakeStage,
                                                                     bool isApplicationOperation,
                                                                     BSONObj bson) noexcept {
    // Initial state: don't drop connections, no immediate check, and don't generate an error server
    // description.
    ErrorActions result;
    ON_BLOCK_EXIT([this, &result, &host, &status] {
        if (result.helloOutcome)
            _clearConsecutiveErrorsWithoutHelloOutcome(host);

        LOGV2(4712102,
              "Host failed in replica set",
              "replicaSet"_attr = _setName,
              "host"_attr = host,
              "error"_attr = status,
              "action"_attr = result);
    });

    // Helpers to mutate the actions
    const auto setCreateServerDescriptionAction = [this, &result, &host, &status, bson]() {
        result.helloOutcome = _createErrorHelloOutcome(host, bson, status);
    };
    const auto setImmediateCheckAction = [&result]() { result.requestImmediateCheck = true; };
    const auto setDropConnectionsAction = [&result]() { result.dropConnections = true; };

    if (!_isNetworkError(status) && !_isNotMasterOrNodeRecovering(status)) {
        setCreateServerDescriptionAction();
        return result;
    }

    if (isApplicationOperation) {
        if (_isNetworkError(status)) {
            switch (handshakeStage) {
                case HandshakeStage::kPreHandshake:
                    setCreateServerDescriptionAction();
                    break;
                case HandshakeStage::kPostHandshake:
                    if (!_isNetworkTimeout(status)) {
                        setCreateServerDescriptionAction();
                    }
                    break;
            }
            setDropConnectionsAction();
        } else if (_isNotMasterOrNodeRecovering(status)) {
            setCreateServerDescriptionAction();
            setImmediateCheckAction();
            if (_isNodeShuttingDown(status)) {
                setDropConnectionsAction();
            }
        }
    } else if (_isNetworkError(status)) {
        switch (handshakeStage) {
            case HandshakeStage::kPreHandshake:
                setCreateServerDescriptionAction();
                break;
            case HandshakeStage::kPostHandshake:
                int errorCount = _getConsecutiveErrorsWithoutHelloOutcome(host);
                if (errorCount == 1) {
                    setCreateServerDescriptionAction();
                } else {
                    setImmediateCheckAction();
                    _incrementConsecutiveErrorsWithoutHelloOutcome(host);
                }
                break;
        }
        setDropConnectionsAction();
    }

    return result;
}

BSONObj StreamableReplicaSetMonitorErrorHandler::ErrorActions::toBSON() const {
    BSONObjBuilder builder;
    builder.append("dropConnections", dropConnections);
    builder.append("requestImmediateCheck", requestImmediateCheck);
    if (helloOutcome) {
        builder.append("outcome", helloOutcome->toBSON());
    }
    return builder.obj();
}

bool SdamErrorHandler::_isNodeRecovering(const Status& status) const {
    return ErrorCodes::isA<ErrorCategory::RetriableError>(status.code());
}

bool SdamErrorHandler::_isNetworkTimeout(const Status& status) const {
    return ErrorCodes::isA<ErrorCategory::NetworkTimeoutError>(status.code());
}

bool SdamErrorHandler::_isNodeShuttingDown(const Status& status) const {
    return ErrorCodes::isA<ErrorCategory::ShutdownError>(status.code());
}

bool SdamErrorHandler::_isNetworkError(const Status& status) const {
    return ErrorCodes::isA<ErrorCategory::NetworkError>(status.code());
}

bool SdamErrorHandler::_isNotMasterOrNodeRecovering(const Status& status) const {
    return _isNodeRecovering(status) || _isNotMaster(status);
}

bool SdamErrorHandler::_isNotMaster(const Status& status) const {
    return ErrorCodes::isA<ErrorCategory::NotPrimaryError>(status.code());
}

int SdamErrorHandler::_getConsecutiveErrorsWithoutHelloOutcome(const HostAndPort& host) const {
    stdx::lock_guard lock(_mutex);
    if (auto it = _consecutiveErrorsWithoutHelloOutcome.find(host);
        it != _consecutiveErrorsWithoutHelloOutcome.end()) {
        return it->second;
    }
    return 0;
}

void SdamErrorHandler::_incrementConsecutiveErrorsWithoutHelloOutcome(const HostAndPort& host) {
    stdx::lock_guard lock(_mutex);
    auto [iter, wasEmplaced] = _consecutiveErrorsWithoutHelloOutcome.emplace(host, 1);
    if (!wasEmplaced) {
        ++(iter->second);
    }
}

void SdamErrorHandler::_clearConsecutiveErrorsWithoutHelloOutcome(const HostAndPort& host) {
    stdx::lock_guard lock(_mutex);
    _consecutiveErrorsWithoutHelloOutcome.erase(host);
}
}  // namespace mongo
