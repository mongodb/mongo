// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/key_generator.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/keys_collection_client.h"
#include "mongo/db/keys_collection_document_gen.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/time_proof_service.h"
#include "mongo/db/topology/vector_clock/vector_clock.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/fail_point.h"

#include <utility>
#include <vector>

namespace mongo {

namespace {

MONGO_FAIL_POINT_DEFINE(disableKeyGeneration);

/**
 * Inserts a new key to the keys collection.
 *
 * Note: this relies on the fact that ShardRegistry returns a ShardLocal for config in config
 * servers. In other words, it is relying on the fact that this will always execute the write
 * locally and never remotely even if this node is no longer primary.
 */
Status insertNewKey(OperationContext* opCtx,
                    KeysCollectionClient* client,
                    long long keyId,
                    const std::string& purpose,
                    const LogicalTime& expiresAt) {
    KeysCollectionDocument newKey(keyId);
    newKey.setKeysCollectionDocumentBase(
        {purpose, TimeProofService::generateRandomKey(), expiresAt});
    return client->insertNewKey(opCtx, newKey.toBSON());
}

/**
 * Returns a new LogicalTime with the seconds argument added to it.
 */
LogicalTime addSeconds(const LogicalTime& logicalTime, const Seconds& seconds) {
    return LogicalTime(Timestamp(logicalTime.asTimestamp().getSecs() + seconds.count(), 0));
}

}  // unnamed namespace

KeyGenerator::KeyGenerator(std::string purpose,
                           KeysCollectionClient* client,
                           Seconds keyValidForInterval)
    : _client(client), _purpose(std::move(purpose)), _keyValidForInterval(keyValidForInterval) {}

Status KeyGenerator::generateNewKeysIfNeeded(OperationContext* opCtx) {

    if (MONGO_unlikely(disableKeyGeneration.shouldFail())) {
        return {ErrorCodes::FailPointEnabled, "key generation disabled"};
    }

    const auto currentTime = VectorClock::get(opCtx)->getTime();
    auto keyStatus = _client->getNewInternalKeys(
        opCtx, _purpose, currentTime.clusterTime(), false /* tryUseMajority */);

    if (!keyStatus.isOK()) {
        return keyStatus.getStatus();
    }

    const auto& newKeys = keyStatus.getValue();
    auto keyIter = newKeys.cbegin();

    LogicalTime currentKeyExpiresAt;

    long long keyId = currentTime.clusterTime().asTimestamp().asLL();

    if (keyIter == newKeys.cend()) {
        currentKeyExpiresAt = addSeconds(currentTime.clusterTime(), _keyValidForInterval);
        auto status = insertNewKey(opCtx, _client, keyId, _purpose, currentKeyExpiresAt);

        if (!status.isOK()) {
            return status;
        }

        keyId++;
    } else if (keyIter->getExpiresAt() < currentTime.clusterTime()) {
        currentKeyExpiresAt = addSeconds(currentTime.clusterTime(), _keyValidForInterval);
        auto status = insertNewKey(opCtx, _client, keyId, _purpose, currentKeyExpiresAt);

        if (!status.isOK()) {
            return status;
        }

        keyId++;
        ++keyIter;
    } else {
        currentKeyExpiresAt = keyIter->getExpiresAt();
        ++keyIter;
    }

    // Create a new key in advance if we don't have a key on standby after the current one
    // expires.
    // Note: Convert this block into a loop if more reserved keys are desired.
    if (keyIter == newKeys.cend()) {
        auto reserveKeyExpiresAt = addSeconds(currentKeyExpiresAt, _keyValidForInterval);
        auto status = insertNewKey(opCtx, _client, keyId, _purpose, reserveKeyExpiresAt);

        if (!status.isOK()) {
            return status;
        }
    } else if (keyIter->getExpiresAt() < currentTime.clusterTime()) {
        currentKeyExpiresAt = addSeconds(currentKeyExpiresAt, _keyValidForInterval);
        auto status = insertNewKey(opCtx, _client, keyId, _purpose, currentKeyExpiresAt);

        if (!status.isOK()) {
            return status;
        }
    }

    return Status::OK();
}
}  // namespace mongo
