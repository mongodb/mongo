/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/key_generator.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/keys_collection_client.h"
#include "mongo/db/keys_collection_document_gen.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/time_proof_service.h"
#include "mongo/db/vector_clock/vector_clock.h"
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
