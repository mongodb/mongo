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

#pragma once

#include <string>

#include "mongo/util/duration.h"

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
