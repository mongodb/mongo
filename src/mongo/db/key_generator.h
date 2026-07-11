// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"

#include <string>

namespace mongo {

class OperationContext;
class KeysCollectionClient;

/**
 * Checks to make sure that there will be valid keys available to sign the current cluster time,
 * and that there will be another key ready after the current key expires. Generates keys if they
 * are necessary.
 *
 * Assumptions and limitations:
 * - assumes that user does not manually update the keys collection.
 * - assumes that current process is the config primary.
 */
class KeyGenerator {
public:
    KeyGenerator(std::string purpose, KeysCollectionClient* client, Seconds keyValidForInterval);
    ~KeyGenerator() = default;

    /**
     * Check if there are new documents expiresAt > latestKeyDoc.expiresAt.
     */
    Status generateNewKeysIfNeeded(OperationContext* opCtx);

private:
    KeysCollectionClient* const _client;
    const std::string _purpose;
    const Seconds _keyValidForInterval;
};

}  // namespace mongo
