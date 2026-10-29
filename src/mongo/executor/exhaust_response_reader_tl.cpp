// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/executor/exhaust_response_reader_tl.h"

#include "mongo/base/error_codes.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/executor/async_client_factory.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"

#include <mutex>

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

namespace mongo {
namespace executor {

ExhaustResponseReaderTL::InFlightReadCanceller::Read
ExhaustResponseReaderTL::InFlightReadCanceller::startRead() {
    auto source = std::make_shared<CancellationSource>();
    bool done;
    uint64_t id;
    {
        std::lock_guard lk(_mutex);
        invariant(!_inFlight);
        _inFlight = source;
        id = _inFlightId = ++_lastId;
        done = _canceled || _finished;
    }
    if (done) {
        source->cancel();
    }
    return {source->token(), id};
}

void ExhaustResponseReaderTL::InFlightReadCanceller::finishRead(uint64_t id) {
    _consumeInFlight(id);
}

void ExhaustResponseReaderTL::InFlightReadCanceller::cancelRead(uint64_t id) {
    if (auto source = _consumeInFlight(id)) {
        source->cancel();
    }
}

void ExhaustResponseReaderTL::InFlightReadCanceller::cancel() {
    std::shared_ptr<CancellationSource> source;
    {
        std::lock_guard lk(_mutex);
        if (_finished) {
            return;
        }
        _canceled = true;
        source = _inFlight;
    }
    if (source) {
        source->cancel();
    }
}

void ExhaustResponseReaderTL::InFlightReadCanceller::finish() {
    std::shared_ptr<CancellationSource> source;
    {
        std::lock_guard lk(_mutex);
        _finished = true;
        source = std::move(_inFlight);
    }
}

std::shared_ptr<CancellationSource>
ExhaustResponseReaderTL::InFlightReadCanceller::_consumeInFlight(uint64_t id) {
    std::lock_guard lk(_mutex);
    return id == _inFlightId ? std::move(_inFlight) : nullptr;
}

ExhaustResponseReaderTL::ExhaustResponseReaderTL(
    RemoteCommandRequest originatingRequest,
    RemoteCommandResponse initialResponse,
    std::shared_ptr<AsyncClientFactory::AsyncClientHandle> client,
    std::shared_ptr<Baton> baton,
    std::shared_ptr<transport::Reactor> reactor,
    const CancellationToken& token)
    : _originatingRequest(std::move(originatingRequest)),
      _initialResponse(std::move(initialResponse)),
      _client(std::move(client)),
      _baton(std::move(baton)),
      _reactor(std::move(reactor)),
      _cancelSource(token),
      _inFlightReadCanceller(std::make_shared<InFlightReadCanceller>()),
      _timer(_reactor->makeTimer()) {

    // Runs inline on whichever thread cancels the stream. That is acceptable because the callback
    // only logs and calls `InFlightReadCanceller::cancel()`, which takes a short-lived mutex and
    // then cancels the in-flight read's source outside the lock. Cancelling that source may in
    // turn run the read's cancellation callbacks inline, so they must not block.
    _cancelSource.token().onCancel().unsafeToInlineFuture().getAsync(
        [request = _originatingRequest, canceller = _inFlightReadCanceller](Status s) {
            if (!s.isOK()) {
                return;
            }

            LOGV2_DEBUG(9311301,
                        2,
                        "Cancelling exhaust command",
                        "requestId"_attr = request.id,
                        "request"_attr = redact(request.toString()));
            canceller->cancel();
        });
}

ExhaustResponseReaderTL::~ExhaustResponseReaderTL() {
    if (_finished.load()) {
        return;
    }
    _recordConnectionOutcome(Status(ErrorCodes::CallbackCanceled,
                                    "Exhaust command cancelled before all responses were read"));
}

void ExhaustResponseReaderTL::_recordConnectionOutcome(Status outcome) {
    if (_finished.swap(true)) {
        return;
    }
    // The stream is over: stop forwarding stream-level cancellation and release any in-flight
    // read's source.
    _inFlightReadCanceller->finish();

    LOGV2_DEBUG(9311300,
                2,
                "Finished reading exhaust responses",
                "requestId"_attr = _originatingRequest.id,
                "outcome"_attr = outcome);

    _client->indicateUsed();
    if (outcome.isOK()) {
        _client->indicateSuccess();
    } else {
        _client->indicateFailure(std::move(outcome));
    }
}

Future<RemoteCommandResponse> ExhaustResponseReaderTL::_read(const CancellationToken& token) {
    if (_initialResponse) {
        return std::exchange(_initialResponse, {}).value();
    } else if (_finished.load()) {
        return Status(ErrorCodes::ExhaustCommandFinished,
                      "No further exhaust responses available to read");
    }
    return _client->getClient().awaitExhaustCommand(_baton, token);
}

SemiFuture<RemoteCommandResponse> ExhaustResponseReaderTL::next() {
    // Each read gets its own short-lived token from `_inFlightReadCanceller` (SERVER-135020)
    // rather than a child of `_cancelSource`, which would register with the stream token and
    // accumulate there. The local timeout below cancels only this read, so a timer firing after
    // the response was read cannot cancel the stream.
    auto read = _inFlightReadCanceller->startRead();

    if (_originatingRequest.timeout != RemoteCommandRequest::kNoTimeout &&
        _originatingRequest.enforceLocalTimeout) {
        auto deadline = _reactor->now() + _originatingRequest.timeout;

        _timer->waitUntil(deadline, _baton)
            .getAsync([deadline,
                       requestId = _originatingRequest.id,
                       canceller = _inFlightReadCanceller,
                       readId = read.id](Status s) {
                if (!s.isOK()) {
                    return;
                }

                LOGV2_DEBUG(9311302,
                            2,
                            "Reading exhaust response timed out, cancelling",
                            "requestId"_attr = requestId,
                            "deadline"_attr = deadline);
                canceller->cancelRead(readId);
            });
    }

    return _read(read.token)
        .onCompletion([this, readId = read.id](StatusWith<RemoteCommandResponse> swResp)
                          -> StatusWith<RemoteCommandResponse> {
            _timer->cancel(_baton);
            _inFlightReadCanceller->finishRead(readId);

            if (swResp.isOK()) {
                return swResp;
            }
            return RemoteCommandResponse(_originatingRequest.target, std::move(swResp.getStatus()));
        })
        .tapAll(
            [this, anchor = shared_from_this()](const StatusWith<RemoteCommandResponse>& swResp) {
                invariant(swResp);

                auto& resp = swResp.getValue();
                if (!resp.moreToCome) {
                    // Once the remote has indicated it has no more responses to send, we can
                    // record the outcome.
                    _recordConnectionOutcome(resp.status);
                }
            })
        .semi();
}
}  // namespace executor
}  // namespace mongo
