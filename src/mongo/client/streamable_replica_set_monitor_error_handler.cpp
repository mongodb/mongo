// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/client/streamable_replica_set_monitor_error_handler.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/logv2/log.h"
#include "mongo/util/scopeguard.h"

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork


namespace mongo {

SdamErrorHandler::ErrorActions SdamErrorHandler::computeErrorActions(const HostAndPort& host,
                                                                     const Status& status,
                                                                     TriggerEvent triggerEvent,
                                                                     BSONObj bson) {
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

    bool isApplicationOperation = isApplicationEvent(triggerEvent);
    bool isFailedRemoteCheck = _isRemoteError(bson) && !isApplicationOperation;

    // Helpers to mutate the actions
    const auto setCreateServerDescriptionAction = [this, &result, &host, &status, bson]() {
        result.helloOutcome = _createErrorHelloOutcome(host, bson, status);
    };
    const auto setImmediateCheckAction = [&result]() {
        result.requestImmediateCheck = true;
    };
    const auto setDropConnectionsAction = [&result]() {
        result.dropConnections = true;
    };

    // If the failure is not retriable, then signal to create a new server description. Currently,
    // all NotPrimary and Network errors are retriable so they are redundant here, but we include
    // them just in case that ever changes in the future.
    if (!_isRetriableError(status) && !_isNotPrimaryError(status) && !_isNetworkError(status)) {
        setCreateServerDescriptionAction();
        return result;
    }

    if (isFailedRemoteCheck) {
        setCreateServerDescriptionAction();
    } else if (isApplicationOperation) {
        if (_isNetworkError(status)) {
            if (isPreHandshakeEvent(triggerEvent)) {
                setCreateServerDescriptionAction();
            } else if (isPostHandshakeEvent(triggerEvent) && !_isNetworkTimeout(status)) {
                setCreateServerDescriptionAction();
            }
            setDropConnectionsAction();
        }
        // Currently, all NotPrimary errors are retriable so it's redundant here, but we include it
        // just in case that ever changes in the future.
        else if (_isRetriableError(status) || _isNotPrimaryError(status)) {
            setCreateServerDescriptionAction();
            setImmediateCheckAction();
            if (_isNodeShuttingDown(status)) {
                setDropConnectionsAction();
            }
        }
    } else if (_isNetworkError(status)) {
        if (isPreHandshakeEvent(triggerEvent)) {
            setCreateServerDescriptionAction();
        } else if (isPostHandshakeEvent(triggerEvent)) {
            int errorCount = _getConsecutiveErrorsWithoutHelloOutcome(host);
            if (errorCount == 1) {
                setCreateServerDescriptionAction();
            } else {
                setImmediateCheckAction();
                _incrementConsecutiveErrorsWithoutHelloOutcome(host);
            }
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

bool SdamErrorHandler::_isRetriableError(const Status& status) const {
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

bool SdamErrorHandler::_isNotPrimaryError(const Status& status) const {
    return ErrorCodes::isA<ErrorCategory::NotPrimaryError>(status.code());
}

bool SdamErrorHandler::_isRemoteError(const BSONObj& bson) const {
    const auto codeElem = bson["ok"];
    return !codeElem.eoo() && !codeElem.trueValue();
}

int SdamErrorHandler::_getConsecutiveErrorsWithoutHelloOutcome(const HostAndPort& host) const {
    std::lock_guard lock(_mutex);
    if (auto it = _consecutiveErrorsWithoutHelloOutcome.find(host);
        it != _consecutiveErrorsWithoutHelloOutcome.end()) {
        return it->second;
    }
    return 0;
}

void SdamErrorHandler::_incrementConsecutiveErrorsWithoutHelloOutcome(const HostAndPort& host) {
    std::lock_guard lock(_mutex);
    auto [iter, wasEmplaced] = _consecutiveErrorsWithoutHelloOutcome.emplace(host, 1);
    if (!wasEmplaced) {
        ++(iter->second);
    }
}

void SdamErrorHandler::_clearConsecutiveErrorsWithoutHelloOutcome(const HostAndPort& host) {
    std::lock_guard lock(_mutex);
    _consecutiveErrorsWithoutHelloOutcome.erase(host);
}
}  // namespace mongo
