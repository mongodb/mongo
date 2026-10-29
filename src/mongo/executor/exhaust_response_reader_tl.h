// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/baton.h"
#include "mongo/executor/async_client_factory.h"
#include "mongo/executor/network_interface.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <memory>
#include <mutex>

namespace mongo::executor {

/**
 * An ExhaustResponseReader implementation based on sessions provided from a TransportLayer.
 * These are created and returned from NetworkInterfaceTL's exhaust path.
 *
 * The underlying session is returned to the pool upon destruction of the ExhaustResponseReader.
 */
class [[MONGO_MOD_PUBLIC]] ExhaustResponseReaderTL
    : public NetworkInterface::ExhaustResponseReader {
public:
    /**
     * Forwards stream-level cancellation to only the read currently in flight (SERVER-135020).
     *
     * Each read's async operation attaches cancellation state to the token it is given.
     * Passing the long-lived stream token to every read would accumulate that state for
     * the life of the exhaust command (which can be forever).
     *
     * Instead, this class owns a short-lived `CancellationSource` for the read in flight, and
     * `cancel` forwards stream-level cancellation to it. Reads are identified by an id so a
     * late local-timeout callback cannot cancel a later read.
     */
    class InFlightReadCanceller {
    public:
        struct Read {
            CancellationToken token;
            uint64_t id;
        };

        /**
         * Creates the short-lived `CancellationSource` for a new read and returns its token.
         * Any read after the stream was cancelled is cancelled immediately.
         */
        Read startRead();

        /** Releases the read's source. Does nothing if `id` is not the read in flight. */
        void finishRead(uint64_t id);

        /** Cancels only the read `id`, if it is still in flight. */
        void cancelRead(uint64_t id);

        /** Stream-level cancellation: cancels the read in flight, and any read started later. */
        void cancel();

        /** After `finish`, `cancel` is a no-op and no source is retained. */
        void finish();

    private:
        /** Removes and returns the in-flight source if it belongs to read `id`, else null. */
        std::shared_ptr<CancellationSource> _consumeInFlight(uint64_t id);

        std::mutex _mutex;
        bool _canceled = false;
        bool _finished = false;
        uint64_t _lastId = 0;
        uint64_t _inFlightId = 0;
        std::shared_ptr<CancellationSource> _inFlight;
    };

    /**
     * Cancelling the `token` cancels the read currently in flight (its future resolves with
     * `CallbackCanceled`), and any later `next` call fails with `ExhaustCommandFinished`.
     * A local timeout (if enforced) cancels only the read it was started for.
     */
    ExhaustResponseReaderTL(RemoteCommandRequest originalRequest,
                            RemoteCommandResponse initialResponse,
                            std::shared_ptr<AsyncClientFactory::AsyncClientHandle> conn,
                            std::shared_ptr<Baton> baton,
                            std::shared_ptr<transport::Reactor> reactor,
                            const CancellationToken& token = CancellationToken::uncancelable());

    ~ExhaustResponseReaderTL() override;
    SemiFuture<RemoteCommandResponse> next() final;

    [[MONGO_MOD_PUBLIC]] size_t getNumCancellationCallbacks_forTest() const {
        return _cancelSource.getNumCancellationCallbacks_forTest();
    }

private:
    void _recordConnectionOutcome(Status outcome);

    Future<RemoteCommandResponse> _read(const CancellationToken& token);

    RemoteCommandRequest _originatingRequest;
    boost::optional<RemoteCommandResponse> _initialResponse;

    Atomic<bool> _finished{false};
    std::shared_ptr<AsyncClientFactory::AsyncClientHandle> _client;

    std::shared_ptr<Baton> _baton;
    std::shared_ptr<transport::Reactor> _reactor;
    CancellationSource _cancelSource;
    std::shared_ptr<InFlightReadCanceller> _inFlightReadCanceller;
    std::unique_ptr<transport::ReactorTimer> _timer;
};
}  // namespace mongo::executor
