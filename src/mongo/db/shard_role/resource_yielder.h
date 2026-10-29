// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Functions to call before and after blocking on the network layer so that resources may be given
 * up so that "sub operations" running on the same node may use them. For example, a node may want
 * to check in its session before waiting on the network so that a sub-operation may check it out
 * and use it. This is important for preventing deadlocks.
 */
class [[MONGO_MOD_OPEN]] ResourceYielder {
public:
    virtual ~ResourceYielder() = default;

    virtual void yield(OperationContext*) = 0;
    virtual void unyield(OperationContext*) = 0;

    Status yieldNoThrow(OperationContext* opCtx) noexcept {
        try {
            yield(opCtx);
        } catch (const DBException& e) {
            return e.toStatus();
        }
        return Status::OK();
    };

    virtual Status unyieldNoThrow(OperationContext* opCtx) noexcept {
        try {
            unyield(opCtx);
        } catch (const DBException& e) {
            return e.toStatus();
        }
        return Status::OK();
    };
};

/**
 * Helper to run the given callback after yielding the given ResourceYielder and will always
 * unyield if yielding succeeded. If the callback and unyielding both throw errors, the unyield
 * error will be thrown.
 */
template <typename Func>
[[MONGO_MOD_PUBLIC]] std::invoke_result_t<Func> runWithYielding(OperationContext* opCtx,
                                                                ResourceYielder* yielder,
                                                                Func fn) {
    if (yielder) {
        yielder->yield(opCtx);
    }

    auto sw = [&]() -> StatusWith<std::invoke_result_t<Func>> {
        try {
            return fn();
        } catch (const DBException& ex) {
            return ex.toStatus();
        }
    }();

    if (yielder) {
        yielder->unyield(opCtx);
    }

    return uassertStatusOK(sw);
}

}  // namespace mongo
