/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/base/status_with.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/rwmutex.h"
#include "mongo/stdx/chrono.h"
#include "mongo/stdx/future.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/modules.h"

#include <vector>


namespace MONGO_MOD_PUB mongo {
namespace rss {
namespace consensus {

/**
 * Implementation of the Intent Registration system, used by operations to declare intents which are
 * required for access into the Storage layer.
 */

class ReplicationStateTransitionGuard {
    friend class IntentRegistry;
    std::function<void()> _releaseCallback;
    ReplicationStateTransitionGuard(std::function<void()> cb) : _releaseCallback(cb) {}

public:
    ReplicationStateTransitionGuard() = default;
    ReplicationStateTransitionGuard(const ReplicationStateTransitionGuard&) = delete;
    ReplicationStateTransitionGuard(ReplicationStateTransitionGuard&& other) noexcept
        : _releaseCallback(std::move(other._releaseCallback)) {
        other._releaseCallback = nullptr;
    };
    ReplicationStateTransitionGuard& operator=(const ReplicationStateTransitionGuard&) = delete;
    ReplicationStateTransitionGuard& operator=(ReplicationStateTransitionGuard&& other) noexcept {
        _releaseCallback = std::move(other._releaseCallback);
        other._releaseCallback = nullptr;
        return *this;
    }

    void release() {
        if (_releaseCallback) {
            _releaseCallback();
            _releaseCallback = nullptr;
        }
    }
    ~ReplicationStateTransitionGuard() {
        release();
    }
};

class IntentRegistry {
    friend class IntentRegistryTest;

public:
    enum class Intent {
        Read,
        Write,
        LocalWrite,
        BlockingWrite,
        _NumDistinctIntents_,
    };

    enum class InterruptionType {
        StepUp,
        StepDown,
        Rollback,
        Shutdown,
        None,
    };

    /**
     * Class used to represent a unique Intent for a specific operation.
     */
    class IntentToken {
        friend class IntentRegistry;
        using idType = uint64_t;

    public:
        IntentToken(Intent intent);
        idType id() const;
        Intent intent() const;

    private:
        static inline AtomicWord<idType> _currentTokenId = {};
        Intent _intent;
        idType _id;
    };

    /**
     * Creates the IntentRegistry to hold and manage all IntentTokens.
     */
    IntentRegistry();

    static IntentRegistry& get(ServiceContext* serviceContext);
    static IntentRegistry& get(OperationContext* opCtx);

    /**
     * Validates that the intent is compatible with the current system state and
     * registers intent in IntentRegistry, or rejects the request if it is
     * incompatible. This function can throw an exception if the intent cannot
     * be registered.
     */
    IntentToken registerIntent(Intent intent, OperationContext* opCtx);

    /**
     * Removes the intent from the IntentRegistry. This function can throw an
     * exception if the intent cannot be deregistered.
     */
    void deregisterIntent(IntentToken token);

    /**
     * Checks if the requested intent can be declared, returns true if it can and false if it
     * cannot. Note that there is no guarantee that calling canDeclareIntent followed by
     * registerIntent will be successful. This function should be used if you are declaring intent
     * to temporarily check the state of the system to avoid exception handling.
     */
    bool canDeclareIntent(Intent intent, OperationContext* opCtx);

    /**
     * Returns true if there is an active Replication state transition ongoing, false otherwise.
     */
    bool activeStateTransition() {
        stdx::unique_lock lock(_stateMutex);
        return _interruptionCtx != nullptr;
    }

    /**
     * Returns _interruptionContext if there is an active Replication state transition ongoing,
     * boost::none otherwise.
     */
    boost::optional<OperationContext*> replicationStateTransitionInterruptionCtx() {
        stdx::unique_lock lock(_stateMutex);
        if (_interruptionCtx != nullptr) {
            return _interruptionCtx;
        } else {
            return boost::none;
        }
    }

    /**
     * Provides a way for transition threads to kill operations with intents
     * which conflict with the state transition, and wait for all of those
     * operations to deregister. While active, it will also prevent operations
     * that conflict with the ongoing state transtion from registering their
     * intent, except those that originate from the same OperationContext to allow transition
     * threads to perform necessary work.
     */
    stdx::future<ReplicationStateTransitionGuard> killConflictingOperations(
        InterruptionType interruption,
        OperationContext* opCtx,
        boost::optional<uint32_t> timeout_sec = boost::none);

    /**
     * Updates metrics around user ops when a state transition that kills operations occurs (i.e.
     * step up, step down, rollback, or shutdown). Also logs the metrics.
     */
    void updateAndLogStateTransitionMetrics(IntentRegistry::InterruptionType interrupt,
                                            size_t numOpsKilled) const;

    /**
     * Marks the IntentRegistry enabled and resets the active and last interruption.
     */
    void enable();

    /**
     * Marks the IntentRegistry disabled.
     */
    void disable();

    static std::string intentToString(Intent intent);

    static std::string interruptionToString(InterruptionType interrupt);

    size_t getTotalOpsKilled() const;

    std::vector<size_t> getTotalIntentsDeclared() const;

private:
    struct tokenMap {
        mutable stdx::mutex lock;
        stdx::condition_variable cv;
        absl::flat_hash_map<IntentToken::idType, OperationContext*> map;
    };

    bool _validIntent(Intent intent) const;
    void _killOperationsByIntent(Intent intent, InterruptionType interruption);
    void _waitForDrain(Intent intent, stdx::chrono::milliseconds timeout);

    bool _enabled = true;
    RWMutex _stateMutex;
    stdx::condition_variable _activeInterruptionCV;
    InterruptionType _lastInterruption = InterruptionType::None;
    OperationContext* _interruptionCtx = nullptr;
    std::vector<tokenMap> _tokenMaps;
    Atomic<int> _pendingStateChange = 0;
    stdx::condition_variable _pendingStateChangeCV;

    // Tracks number of operations killed on state transition.
    size_t _totalOpsKilled = 0;
};
}  // namespace consensus
}  // namespace rss
}  // namespace MONGO_MOD_PUB mongo
