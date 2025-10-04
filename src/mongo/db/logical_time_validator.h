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

#include "mongo/base/status.h"
#include "mongo/db/auth/cluster_auth_mode.h"
#include "mongo/db/auth/validated_tenancy_scope.h"
#include "mongo/db/keys_collection_document_gen.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/signed_logical_time.h"
#include "mongo/db/time_proof_service.h"
#include "mongo/stdx/mutex.h"

#include <memory>

namespace mongo {

class OperationContext;
class ServiceContext;
class KeysCollectionDocument;
class KeysCollectionManager;

/**
 * This is responsible for signing cluster times that can be used to sent to other servers and
 * verifying signatures of signed cluster times.
 */
class LogicalTimeValidator {
public:
    // Decorate ServiceContext with LogicalTimeValidator instance.
    static std::shared_ptr<LogicalTimeValidator> get(ServiceContext* service);
    static std::shared_ptr<LogicalTimeValidator> get(OperationContext* ctx);
    static void set(ServiceContext* service, std::unique_ptr<LogicalTimeValidator> validator);

    /**
     * Constructs a new LogicalTimeValidator that uses the given key manager. The passed-in
     * key manager must outlive this object.
     */
    explicit LogicalTimeValidator(std::shared_ptr<KeysCollectionManager> keyManager);

    /**
     * Tries to sign the newTime with a valid signature. Can return an empty signature and keyId
     * of 0 if it cannot find valid key for newTime.
     */
    SignedLogicalTime trySignLogicalTime(const LogicalTime& newTime);

    /**
     * Returns the newTime with a valid signature.
     */
    SignedLogicalTime signLogicalTime(OperationContext* opCtx, const LogicalTime& newTime);

    /**
     * Validates the signature of newTime and returns the resulting status.
     */
    Status validate(OperationContext* opCtx, const SignedLogicalTime& newTime);

    /**
     * Initializes this validator. This should be called first before the other methods can be used.
     */
    void init(ServiceContext* service);

    /**
     * Cleans up this validator. This will no longer be usable after this is called.
     */
    void shutDown();

    /**
     * Enable writing new keys to the config server primary. Should only be called if current node
     * is the config primary.
     */
    void enableKeyGenerator(OperationContext* opCtx, bool doEnable);

    /**
     * Returns true if client has sufficient privilege to advance clock.
     */
    static bool isAuthorizedToAdvanceClock(OperationContext* opCtx);

    /**
     * Returns true if the server should gossip, sign, and validate cluster times. False until there
     * are keys in the config server.
     */
    bool shouldGossipLogicalTime();

    /**
     * Stops the key manager and resets its state to prevent the former members of standalone
     * replica set to use old keys with sharded cluster.
     */
    void stopKeyManager();

    /**
     * Load the given external key into the key manager's keys cache.
     */
    void cacheExternalKey(ExternalKeysCollectionDocument key);

    /**
     * Reset the key manager cache of keys.
     */
    void resetKeyManagerCache();

private:
    /**
     *  Returns the copy of the _keyManager to work when its reset by resetKeyManager call.
     */
    std::shared_ptr<KeysCollectionManager> _getKeyManagerCopy();


    SignedLogicalTime _getProof(const KeysCollectionDocument& keyDoc, LogicalTime newTime);

    stdx::mutex _mutex;  // protects _lastSeenValidTime
    SignedLogicalTime _lastSeenValidTime;
    TimeProofService _timeProofService;
    std::shared_ptr<KeysCollectionManager> _keyManager;
};

}  // namespace mongo
