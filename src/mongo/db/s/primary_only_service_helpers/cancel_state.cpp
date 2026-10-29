// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/primary_only_service_helpers/cancel_state.h"

namespace mongo {
namespace primary_only_service_helpers {

CancelState::CancelState(boost::optional<CancellationToken> stepdownToken) {
    if (stepdownToken) {
        attachStepdownToken(*stepdownToken);
    }
}

void CancelState::attachStepdownToken(const CancellationToken& stepdownToken) {
    stepdownToken.onCancel()
        .unsafeToInlineFuture()
        .then([stepdownSource = _stepdownSource,
               abortOrStepdownSource = _abortOrStepdownSource]() mutable {
            stepdownSource.cancel();
            abortOrStepdownSource.cancel();
        })
        .getAsync([](auto) {});
}

CancellationToken CancelState::getStepdownToken() const {
    return _stepdownSource.token();
}

CancellationToken CancelState::getAbortOrStepdownToken() const {
    return _abortOrStepdownSource.token();
}

bool CancelState::isSteppingDown() const {
    return _stepdownSource.token().isCanceled();
}

bool CancelState::isAbortedOrSteppingDown() const {
    return _abortOrStepdownSource.token().isCanceled();
}

void CancelState::abort() {
    _abortOrStepdownSource.cancel();
}

}  // namespace primary_only_service_helpers
}  // namespace mongo
