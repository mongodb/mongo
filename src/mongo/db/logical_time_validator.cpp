// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/logical_time_validator.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/status_with.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/client.h"
#include "mongo/db/keys_collection_manager.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/topology/vector_clock/vector_clock.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/transport/session.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/time_support.h"

#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {

namespace {

MONGO_FAIL_POINT_DEFINE(alwaysValidateClientsClusterTime);
MONGO_FAIL_POINT_DEFINE(externalClientsNeverAuthorizedToAdvanceLogicalClock);
MONGO_FAIL_POINT_DEFINE(throwClientDisconnectInSignLogicalTimeForExternalClients);

const auto getLogicalTimeValidator =
    ServiceContext::declareDecoration<std::shared_ptr<LogicalTimeValidator>>();

std::mutex validatorMutex;  // protects access to decoration instance of
                            // LogicalTimeValidator.

Milliseconds kRefreshIntervalIfErrored(200);

}  // unnamed namespace

std::shared_ptr<LogicalTimeValidator> LogicalTimeValidator::get(ServiceContext* service) {
    std::lock_guard<std::mutex> lk(validatorMutex);
    return getLogicalTimeValidator(service);
}

std::shared_ptr<LogicalTimeValidator> LogicalTimeValidator::get(OperationContext* ctx) {
    return get(ctx->getClient()->getServiceContext());
}

void LogicalTimeValidator::set(ServiceContext* service,
                               std::unique_ptr<LogicalTimeValidator> newValidator) {
    std::lock_guard<std::mutex> lk(validatorMutex);
    auto& validator = getLogicalTimeValidator(service);
    validator = std::move(newValidator);
}

LogicalTimeValidator::LogicalTimeValidator(std::shared_ptr<KeysCollectionManager> keyManager)
    : _keyManager(keyManager) {}

SignedLogicalTime LogicalTimeValidator::_getProof(const KeysCollectionDocument& keyDoc,
                                                  LogicalTime newTime) {
    auto key = keyDoc.getKey();

    // Compare and calculate HMAC inside mutex to prevent multiple threads computing HMAC for the
    // same cluster time.
    std::lock_guard<std::mutex> lk(_mutex);
    // Note: _lastSeenValidTime will initially not have a proof set.
    if (newTime == _lastSeenValidTime.getTime() && _lastSeenValidTime.getProof()) {
        return _lastSeenValidTime;
    }

    auto signature = _timeProofService.getProof(newTime, key);
    SignedLogicalTime newSignedTime(newTime, std::move(signature), keyDoc.getKeyId());

    if (newTime > _lastSeenValidTime.getTime() || !_lastSeenValidTime.getProof()) {
        _lastSeenValidTime = newSignedTime;
    }

    return newSignedTime;
}

SignedLogicalTime LogicalTimeValidator::trySignLogicalTime(const LogicalTime& newTime) {
    auto keyStatusWith = _getKeyManagerCopy()->getKeyForSigning(nullptr, newTime);
    auto keyStatus = keyStatusWith.getStatus();

    if (keyStatus == ErrorCodes::KeyNotFound) {
        // Attach invalid signature and keyId if we don't have the right keys to sign it.
        return SignedLogicalTime(newTime, TimeProofService::TimeProof(), 0);
    }

    uassertStatusOK(keyStatus);
    return _getProof(keyStatusWith.getValue(), newTime);
}

SignedLogicalTime LogicalTimeValidator::signLogicalTime(OperationContext* opCtx,
                                                        const LogicalTime& newTime) {
    auto keyManager = _getKeyManagerCopy();
    auto keyStatusWith = keyManager->getKeyForSigning(nullptr, newTime);
    auto keyStatus = keyStatusWith.getStatus();

    while (keyStatus == ErrorCodes::KeyNotFound && VectorClock::get(opCtx)->isEnabled()) {
        keyManager->refreshNow(opCtx);

        keyStatusWith = keyManager->getKeyForSigning(nullptr, newTime);
        keyStatus = keyStatusWith.getStatus();

        if (keyStatus == ErrorCodes::KeyNotFound) {
            sleepFor(kRefreshIntervalIfErrored);
        }
    }

    if (MONGO_unlikely(throwClientDisconnectInSignLogicalTimeForExternalClients.shouldFail() &&
                       opCtx->getClient()->session() && !opCtx->getClient()->isInternalClient())) {
        // KeysCollectionManager::refreshNow() can throw an exception if the client has
        // already disconnected. We simulate such behavior using this failpoint.
        keyStatus = {ErrorCodes::ClientDisconnect,
                     "throwClientDisconnectInSignLogicalTimeForExternalClients failpoint enabled"};
    }

    uassertStatusOK(keyStatus);
    return _getProof(keyStatusWith.getValue(), newTime);
}

Status LogicalTimeValidator::validate(OperationContext* opCtx, const SignedLogicalTime& newTime) {
    {
        std::lock_guard<std::mutex> lk(_mutex);
        if (newTime.getTime() <= _lastSeenValidTime.getTime() &&
            !MONGO_unlikely(alwaysValidateClientsClusterTime.shouldFail())) {
            return Status::OK();
        }
    }

    auto keyStatusWith =
        _getKeyManagerCopy()->getKeysForValidation(opCtx, newTime.getKeyId(), newTime.getTime());
    auto status = keyStatusWith.getStatus();

    if (!status.isOK()) {
        return status;
    }

    auto keys = keyStatusWith.getValue();
    invariant(!keys.empty());

    const auto newProof = newTime.getProof();
    // Cluster time is only sent if a server's clock can verify and sign cluster times, so any
    // received cluster times should have proofs.
    invariant(newProof);

    auto firstError = Status::OK();
    for (const auto& key : keys) {
        auto proofStatus =
            _timeProofService.checkProof(newTime.getTime(), newProof.value(), key.getKey());
        if (proofStatus.isOK()) {
            return Status::OK();
        } else if (firstError.isOK()) {
            firstError = proofStatus;
        }
    }

    return firstError;
}

void LogicalTimeValidator::init(ServiceContext* service) {
    _getKeyManagerCopy()->startMonitoring(service);
}

void LogicalTimeValidator::shutDown() {
    if (_keyManager) {
        _keyManager->stopMonitoring();
    }
}

void LogicalTimeValidator::enableKeyGenerator(OperationContext* opCtx, bool doEnable) {
    _getKeyManagerCopy()->enableKeyGenerator(opCtx, doEnable);
}

bool LogicalTimeValidator::isAuthorizedToAdvanceClock(OperationContext* opCtx) {
    if (MONGO_unlikely(externalClientsNeverAuthorizedToAdvanceLogicalClock.shouldFail())) {
        return opCtx->getClient()->session() && opCtx->getClient()->isInternalClient();
    }

    auto as = AuthorizationSession::get(opCtx->getClient());
    // Note: returns true if auth is off, courtesy of
    // AuthzSessionExternalStateServerCommon::shouldIgnoreAuthChecks.
    return as->isAuthorizedForClusterAction(ActionType::advanceClusterTime, as->getUserTenantId());
}

bool LogicalTimeValidator::shouldGossipLogicalTime() {
    return _getKeyManagerCopy()->hasSeenKeys();
}

void LogicalTimeValidator::cacheExternalKey(ExternalKeysCollectionDocument key) {
    invariant(_keyManager);
    _keyManager->cacheExternalKey(std::move(key));
}

void LogicalTimeValidator::resetKeyManagerCache() {
    LOGV2(20716, "Resetting key manager cache");
    invariant(_keyManager);
    _keyManager->clearCache();
    std::lock_guard<std::mutex> lk(_mutex);
    _lastSeenValidTime = SignedLogicalTime();
    _timeProofService.resetCache();
}

void LogicalTimeValidator::stopKeyManager() {
    if (_keyManager) {
        LOGV2(20717, "Stopping key manager");
        _keyManager->stopMonitoring();
        _keyManager->clearCache();

        std::lock_guard<std::mutex> lk(_mutex);
        _lastSeenValidTime = SignedLogicalTime();
        _timeProofService.resetCache();
    } else {
        LOGV2(20718, "Stopping key manager: no key manager exists.");
    }
}

std::shared_ptr<KeysCollectionManager> LogicalTimeValidator::_getKeyManagerCopy() {
    invariant(_keyManager);
    return _keyManager;
}

}  // namespace mongo
