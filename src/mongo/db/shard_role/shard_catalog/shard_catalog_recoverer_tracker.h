// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/modules.h"

#include <list>
#include <mutex>
#include <utility>

namespace mongo {

// TODO (SERVER-98118): Re-consider the following class and enum once 9.0 is last-lts and we don't
// need to interrupt incompatible recoveries.

enum class RecoveryKind { kNonAuthoritative, kAuthoritative };

/**
 * Tracks in-flight shard catalog recoveries and can interrupt those incompatible with the
 * current Authoritative Shards FCV.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] ShardCatalogRecovererTracker {
private:
    struct ActiveRecovery {
        RecoveryKind kind;
        CancellationSource source;
    };
    using ActiveList = std::list<ActiveRecovery>;

public:
    static ShardCatalogRecovererTracker* get(OperationContext* opCtx);

    /**
     * RAII registration for one in-flight recovery.
     */
    class Acquisition {
    public:
        Acquisition(const Acquisition&) = delete;
        Acquisition& operator=(const Acquisition&) = delete;
        Acquisition& operator=(Acquisition&&) = delete;

        Acquisition(Acquisition&& other)
            : _tracker(std::exchange(other._tracker, nullptr)), _it(other._it) {}

        ~Acquisition() {
            if (_tracker) {
                _tracker->_release(_it);
            }
        }

        void cancel() {
            if (_tracker) {
                _it->source.cancel();
            }
        }

        RecoveryKind kind() const {
            return _it->kind;
        }

        CancellationToken cancellationToken() const {
            return _it->source.token();
        }

    private:
        friend class ShardCatalogRecovererTracker;

        Acquisition(ShardCatalogRecovererTracker* tracker, ActiveList::iterator it)
            : _tracker(tracker), _it(it) {}

        ShardCatalogRecovererTracker* _tracker = nullptr;
        ActiveList::iterator _it;
    };

    /**
     * Marks that the current thread is doing a shard catalog recovery. The recovery kind is
     * selected automatically based on the Authoritative Shards feature flag.
     */
    Acquisition acquire(OperationContext* opCtx);

    /**
     * Cancels in-flight recoveries that are incompatible with the current value of the
     * Authoritative Shards feature flag (i.e. cancels non-authoritative recoveries when the flag
     * is enabled, authoritative recoveries when it is disabled) and waits until they drain.
     */
    void interruptIncompatibleRecoveries(OperationContext* opCtx);

private:
    void _release(ActiveList::iterator it);

    std::mutex _mutex;
    // Notified when a canceled recovery is released, so we can wait for them to drain.
    stdx::condition_variable _canceled;
    // Cancellation source for each ongoing shard catalog recovery. We do not use a single
    // "root" CancellationSource in order to avoid the memory leak described in SERVER-92333.
    ActiveList _activeRecoveries;
};

}  // namespace mongo
