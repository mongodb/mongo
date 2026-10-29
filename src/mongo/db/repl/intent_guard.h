// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/repl/intent_registry.h"
#include "mongo/util/modules.h"

#include <boost/optional.hpp>

namespace mongo::rss::consensus {

/**
 * RAII class used to handle registering and deregistering intents with the IntentRegistration
 * system.
 */
class [[MONGO_MOD_PUBLIC]] IntentGuard {
public:
    /**
     * Registers an intent. If the intent is not granted, an exception will be thrown. It will be up
     * to the caller to handle the case when the intent is not granted.
     */
    IntentGuard(IntentRegistry::Intent intent, OperationContext* opctx);

    IntentGuard(const IntentGuard&) = delete;
    IntentGuard& operator=(const IntentGuard&) = delete;

    IntentGuard(IntentGuard&& o) noexcept
        : _opCtx(std::exchange(o._opCtx, {})),
          _svcCtx(std::exchange(o._svcCtx, {})),
          _token(o._token) {}

    IntentGuard& operator=(IntentGuard&& o) noexcept {
        if (this != &o) {
            _opCtx = std::exchange(o._opCtx, {});
            _svcCtx = std::exchange(o._svcCtx, {});
            _token = o._token;
        }
        return *this;
    }

    /**
     * Deregisters the intent.
     */
    ~IntentGuard() {
        reset();
    }

    OperationContext* getOperationContext() const {
        return _opCtx;
    }

    /**
     * Returns the type of intent that was granted, or boost::none if no intent was granted.
     */
    boost::optional<IntentRegistry::Intent> intent() const;

    /**
     * Deregisters the intent but without destructing the IntentGuard object.
     */
    void reset();

private:
    OperationContext* _opCtx;
    // Stored separately so reset() can find the registry even when _opCtx is stale (e.g.,
    // when this guard is deferred to WUOW end and the original opCtx has been destroyed).
    ServiceContext* _svcCtx = nullptr;
    IntentRegistry::IntentToken _token;
};

/**
 * A wrapper class around IntentGuard that is only used for Write Intents.
 */
class [[MONGO_MOD_PUBLIC]] WriteIntentGuard : public IntentGuard {
public:
    explicit WriteIntentGuard(OperationContext* opCtx)
        : IntentGuard(IntentRegistry::Intent::Write, opCtx) {}
};
}  // namespace mongo::rss::consensus
