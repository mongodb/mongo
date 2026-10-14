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
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"

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

// Bridges the stream-level token to only the read that is currently in flight.
struct ExhaustResponseReaderTL::ReadCancellationState {
    void startRead(const std::shared_ptr<CancellationSource>& source) {
        bool canceled;
        {
            stdx::lock_guard lk(_mutex);
            _activeSource = source;
            canceled = _canceled;
        }

        if (canceled) {
            source->cancel();
        }
    }

    void finishRead(const std::shared_ptr<CancellationSource>& source) {
        stdx::lock_guard lk(_mutex);
        if (_activeSource.lock() == source) {
            _activeSource.reset();
        }
    }

    void cancel() {
        std::shared_ptr<CancellationSource> activeSource;
        {
            stdx::lock_guard lk(_mutex);
            if (_finished) {
                return;
            }
            _canceled = true;
            activeSource = _activeSource.lock();
        }

        if (activeSource) {
            activeSource->cancel();
        }
    }

    void finish() {
        stdx::lock_guard lk(_mutex);
        _finished = true;
        _activeSource.reset();
    }

private:
    stdx::mutex _mutex;
    bool _canceled = false;
    bool _finished = false;
    std::weak_ptr<CancellationSource> _activeSource;
};

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
      _readCancellationState(std::make_shared<ReadCancellationState>()),
      _timer(_reactor->makeTimer()) {

    _cancelSource.token().onCancel().unsafeToInlineFuture().getAsync(
        [request = _originatingRequest, state = _readCancellationState](Status s) {
            if (!s.isOK()) {
                return;
            }

            LOGV2_DEBUG(9311301,
                        2,
                        "Cancelling exhaust command",
                        "requestId"_attr = request.id,
                        "request"_attr = redact(request.toString()));
            state->cancel();
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
    _readCancellationState->finish();

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
    // A short-lived source prevents each AsyncDBClient operation from attaching its cancellation
    // state directly to the long-lived exhaust stream token.
    auto source = std::make_shared<CancellationSource>();
    _readCancellationState->startRead(source);

    if (_originatingRequest.timeout != RemoteCommandRequest::kNoTimeout &&
        _originatingRequest.enforceLocalTimeout) {
        auto deadline = _reactor->now() + _originatingRequest.timeout;

        _timer->waitUntil(deadline, _baton)
            .getAsync([deadline,
                       requestId = _originatingRequest.id,
                       source](Status s) {
                if (!s.isOK()) {
                    return;
                }

                LOGV2_DEBUG(9311302,
                            2,
                            "Reading exhaust response timed out, cancelling",
                            "requestId"_attr = requestId,
                            "deadline"_attr = deadline);
                source->cancel();
            });
    }

    return _read(source->token())
        .onCompletion(
            [this, source](
                StatusWith<RemoteCommandResponse> swResp) -> StatusWith<RemoteCommandResponse> {
                _timer->cancel(_baton);
                _readCancellationState->finishRead(source);

                if (swResp.isOK()) {
                    return swResp;
                }
                return RemoteCommandResponse(_originatingRequest.target,
                                             std::move(swResp.getStatus()));
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
