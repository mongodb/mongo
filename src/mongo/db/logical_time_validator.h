// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/auth/cluster_auth_mode.h"
#include "mongo/db/auth/validated_tenancy_scope.h"
#include "mongo/db/keys_collection_document_gen.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/signed_logical_time.h"
#include "mongo/db/time_proof_service.h"
#include "mongo/util/modules.h"

#include <memory>
#include <mutex>

namespace mongo {

class OperationContext;
class ServiceContext;
class KeysCollectionDocument;
class KeysCollectionManager;

/**
 * This is responsible for signing cluster times that can be used to sent to other servers and
 * verifying signatures of signed cluster times.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] LogicalTimeValidator {
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

    std::mutex _mutex;  // protects _lastSeenValidTime
    SignedLogicalTime _lastSeenValidTime;
    TimeProofService _timeProofService;
    std::shared_ptr<KeysCollectionManager> _keyManager;
};

}  // namespace mongo
