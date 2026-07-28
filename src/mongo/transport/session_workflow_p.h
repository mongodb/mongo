// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/db/dbmessage.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/protocol.h"
#include "mongo/transport/session.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/duration.h"
#include "mongo/util/interruptible.h"

#include <utility>

namespace mongo::transport {

/** Build a `DbResponse` carrying a rate-limit-rejection error for `message`. */
DbResponse makeDbResponseErrorForRateLimiting(const Message& message);

/**
 * Per-session `Interruptible` used to throttle IRRL rejection replies on the egress response
 * rate-limiter path. The rejection path has no `OperationContext`, so it cannot use
 * `markKillOnClientDisconnect()`. Instead, this class composes two cancellation conditions in a
 * sliced wait loop:
 *   1. Shutdown: `shutdownToken` from `SessionManager::getShutdownToken()` is polled at each slice
 *      boundary.
 *   2. Client disconnect: each slice is a blocking `poll(2)` for POLLRDHUP/POLLHUP on the
 *      session's socket via `Session::waitForPeerDisconnectUntil()`, so the worker wakes within
 *      one scheduler tick of the client's FIN/RST.
 *
 * Each poll is capped at `kMaxPollInterval` (500ms) so a parked waiter notices shutdown within
 * that bound.
 *
 * Returns `InterruptedAtShutdown` on shutdown, `ClientDisconnect` on peer disconnect.
 */
class DisconnectShutdownAwareInterruptible final : public Interruptible {
public:
    DisconnectShutdownAwareInterruptible(CancellationToken shutdownToken,
                                         Session* session,
                                         ClockSource* clockSource)
        : _shutdownToken(std::move(shutdownToken)), _session(session), _clockSource(clockSource) {}

    StatusWith<stdx::cv_status> waitForConditionOrInterruptNoAssertUntil(
        stdx::condition_variable& callerCv,
        BasicLockableAdapter callerM,
        Date_t deadline) noexcept override {
        // `callerCv` and `callerM` are intentionally ignored. The only consumer is the
        // EgressResponseRateLimiter's sleepUntil()-style wait, whose local CV is never notified.

        if (auto s = _checkShutdown(); !s.isOK()) {
            return s;
        }

        while (true) {
            const auto now = _clockSource->now();
            if (now >= deadline) {
                return stdx::cv_status::timeout;
            }

            const auto sliceDeadline = std::min(deadline, now + kMaxPollInterval);
            if (_session->waitForPeerDisconnectUntil(sliceDeadline)) {
                return Status(ErrorCodes::ClientDisconnect,
                              "client disconnected while waiting in egress response rate limiter "
                              "queue");
            }

            if (auto s = _checkShutdown(); !s.isOK()) {
                return s;
            }
        }
    }

    Date_t getDeadline() const override {
        return Date_t::max();
    }

    Status checkForInterruptNoAssert() noexcept override {
        if (auto s = _checkShutdown(); !s.isOK()) {
            return s;
        }
        if (!_session->isConnected()) {
            return Status(
                ErrorCodes::ClientDisconnect,
                "client disconnected while waiting in egress response rate limiter queue");
        }
        return Status::OK();
    }

    Status checkForDeadlineExpiredNoAssert(Date_t) noexcept override {
        return Status::OK();
    }

    // The only caller (sleepUntil-style wait) never installs a DeadlineGuard, so wiring one up is
    // a programming error; fail loudly rather than silently ignoring an unenforced deadline.
    DeadlineState pushArtificialDeadline(Date_t, ErrorCodes::Error) override {
        MONGO_UNREACHABLE_TASSERT(9297601);
    }

    void popArtificialDeadline(DeadlineState) override {
        MONGO_UNREACHABLE_TASSERT(9297602);
    }

    Date_t getExpirationDateForWaitForValue(Milliseconds waitFor) override {
        return _clockSource->now() + waitFor;
    }

private:
    Status _checkShutdown() noexcept {
        if (_shutdownToken.isCanceled()) {
            return Status(ErrorCodes::InterruptedAtShutdown,
                          "egress response interrupted at shutdown");
        }
        return Status::OK();
    }

    CancellationToken _shutdownToken;
    Session* const _session;
    ClockSource* const _clockSource;

    // Upper bound on a single poll() slice, which bounds how long a parked waiter can stay
    // in poll() before noticing SessionManager shutdown.
    static constexpr Milliseconds kMaxPollInterval{500};
};

}  // namespace mongo::transport
