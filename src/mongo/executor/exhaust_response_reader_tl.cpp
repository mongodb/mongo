/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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
      _timer(_reactor->makeTimer()) {

    // Safe to run the callback inline here since all it does is log.
    _cancelSource.token().onCancel().unsafeToInlineFuture().getAsync(
        [request = _originatingRequest](Status s) {
            if (!s.isOK()) {
                return;
            }

            LOGV2_DEBUG(9311301,
                        2,
                        "Cancelling exhaust command",
                        "requestId"_attr = request.id,
                        "request"_attr = redact(request.toString()));
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

Future<RemoteCommandResponse> ExhaustResponseReaderTL::_read() {
    if (_initialResponse) {
        return std::exchange(_initialResponse, {}).value();
    } else if (_finished.load()) {
        return Status(ErrorCodes::ExhaustCommandFinished,
                      "No further exhaust responses available to read");
    }
    return _client->getClient().awaitExhaustCommand(_baton, _cancelSource.token());
}

SemiFuture<RemoteCommandResponse> ExhaustResponseReaderTL::next() {
    auto token = _cancelSource.token();

    if (_originatingRequest.timeout != RemoteCommandRequest::kNoTimeout &&
        _originatingRequest.enforceLocalTimeout) {
        // Make a new CancellationSource so that if this timer happens to fire after the response
        // has been successfully read, _cancelSource isn't cancelled.
        CancellationSource source(_cancelSource);
        token = source.token();

        auto deadline = _reactor->now() + _originatingRequest.timeout;

        _timer->waitUntil(deadline, _baton)
            .getAsync([deadline,
                       requestId = _originatingRequest.id,
                       weak_self = weak_from_this(),
                       source = std::move(source)](Status s) mutable {
                if (!s.isOK()) {
                    return;
                }

                LOGV2_DEBUG(9311302,
                            2,
                            "Reading exhaust response timed out, cancelling",
                            "requestId"_attr = requestId,
                            "deadline"_attr = deadline);
                source.cancel();
            });
    }

    return _read()
        .onCompletion(
            [this](StatusWith<RemoteCommandResponse> swResp) -> StatusWith<RemoteCommandResponse> {
                _timer->cancel(_baton);

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
