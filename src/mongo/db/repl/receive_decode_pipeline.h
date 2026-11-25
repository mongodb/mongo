// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/transport/session.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"

#include <functional>
#include <memory>
#include <utility>

namespace mongo::repl {

/**
 * One exhaust stream on a fixed, authenticated session. Only the receiver reads the socket;
 * workers decode immutable messages, and one fetcher consumes results in wire order.
 * The caller must stop/join before sending another request or reconnecting the client.
 */
class [[MONGO_MOD_PUBLIC]] ReceiveDecodePipeline {
public:
    struct Reply {
        Reply() = default;
        Reply(Reply&&) = default;
        Reply(const Reply&) = delete;
        Reply& operator=(const Reply&) = delete;

        /**
         * The implicit member-wise assignment would replace 'reservation' first and return the old
         * budget while the old message and parsed views are still alive. Move the old contents
         * into a temporary instead, so they are destroyed in declaration-reverse order: parsed,
         * message, then reservation.
         */
        Reply& operator=(Reply&& other) noexcept {
            if (this != &other) {
                Reply old(std::move(*this));
                reservation = std::move(other.reservation);
                message = std::move(other.message);
                parsed = std::move(other.parsed);
                endedReadAhead = other.endedReadAhead;
            }
            return *this;
        }

        // Declared first so the reservation is destroyed after the owned message and views.
        // Keep it alive while the cursor/metadata still own the reply's buffer.
        std::shared_ptr<void> reservation;
        Message message;
        std::unique_ptr<rpc::ReplyInterface> parsed;
        // True if this reply continues the exhaust stream but the receiver stopped reading after
        // it, because its flags were unavailable without full decoding (an unsupported compressor,
        // a noncanonical prefix, or a prefix workspace/work limit).
        // The reply is valid; the owner must abandon the stream and resume without the pipeline.
        bool endedReadAhead = false;
    };

    static std::shared_ptr<ReceiveDecodePipeline> make(std::shared_ptr<transport::Session> session,
                                                       int expectedResponseTo);
    ~ReceiveDecodePipeline();

    ReceiveDecodePipeline(const ReceiveDecodePipeline&) = delete;
    ReceiveDecodePipeline& operator=(const ReceiveDecodePipeline&) = delete;

    /**
     * One consumer only. Checks responseTo again against the cursor's committed wire state.
     * Once a reply fails or its responseTo mismatches, subsequent calls throw the same error;
     * successful replies after that position are never delivered. The caller must still stop it.
     *
     * While the next reply is not ready, calls 'checkInterrupt' about every
     * kInterruptCheckInterval without holding the pipeline mutex, and throws its status if it is
     * not OK. This lets the owner end the wait on conditions the pipeline does not observe, such as
     * a disabled parameter or a canceled executor callback. It does not cancel the pipeline; the
     * caller must still stop(true) it. The interval is only how often the wait is checked: it is
     * not a bound on recovery, since stop() still waits for in-flight decode tasks, and a decode
     * that never returns cannot be interrupted.
     */
    Reply take(int expectedResponseTo, const std::function<Status()>& checkInterrupt = {});

    static constexpr Milliseconds kInterruptCheckInterval{100};

    /**
     * Thread-safe cancellation; never joins. Also interrupts a blocked sourceMessage() while this
     * pipeline still owns the session. Closing is decided by who acquires the close right first:
     * if stop() has already revoked it, cancel() only marks the pipeline as canceled and leaves
     * the session that was handed back to the client open.
     */
    void cancel() noexcept;

    /**
     * Serialized owner calls only, never under the fetcher mutex or from this pipeline's receiver
     * or decode tasks. cancel(), unlike stop(), may run concurrently. false is legal after all
     * replies through the terminal frame have been taken, or after failure/cancellation; a healthy
     * stream stopped mid-stream throws IllegalOperation without joining. Before returning, revokes
     * this
     * pipeline's right to close the session and waits for any close already started by a
     * concurrent cancel() or worker failure. Before returning, releases all internal reply and
     * session references; replies already taken by the consumer keep their buffers and budget.
     *
     * Returns true if the pipeline was canceled or failed before the session was handed back. In
     * that case the session may already be closed and must not be reused as healthy. The result
     * is conservative: a cancel() that was marked but had not yet closed the session also returns
     * true. Repeated calls return the first result, unaffected by a later cancel().
     */
    bool stop(bool abort);

    /** Called after replication producers stop and before transport/storage dependencies disappear.
     */
    static void shutdownPool();

private:
    struct State;
    struct Receiver;
    explicit ReceiveDecodePipeline(std::shared_ptr<transport::Session> session,
                                   int expectedResponseTo);

    std::shared_ptr<State> _state;
    std::unique_ptr<Receiver> _receiver;
    // Result of the first stop(); repeated calls return it unchanged. Fetcher thread only.
    bool _interruptedAtHandoff = false;
};

}  // namespace mongo::repl
