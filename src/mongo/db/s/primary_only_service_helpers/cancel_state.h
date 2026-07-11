// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/cancellation.h"
#include "mongo/util/modules.h"

#include <boost/optional/optional.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {
namespace primary_only_service_helpers {

/**
 * Tracks abort and stepdown cancellation independently for a PrimaryOnlyService instance.
 *
 * Lifecycle
 * ---------
 * The class is designed to be constructed before run() is called, so abort() is always safe to
 * invoke. The stepdown token -- provided by the PrimaryOnlyService framework to run() -- is
 * attached later via attachStepdownToken().
 *
 * Internally, this class owns two CancellationSources: one for stepdown (_stepdownSource) and one
 * for abort-or-stepdown (_abortOrStepdownSource). Tokens vended by getStepdownToken() and
 * getAbortOrStepdownToken() are derived from these owned sources and are therefore valid from
 * construction. This means it is safe to call getStepdownToken() and getAbortOrStepdownToken()
 * before attachStepdownToken() is called; callers will simply hold inert tokens that become
 * cancelled once the real stepdown token is attached and subsequently cancelled.
 *
 * When attachStepdownToken() is called with the real POS stepdown token, it registers an inline
 * callback on that token's onCancel() future. When the node steps down (cancelling the POS token),
 * the callback fires synchronously and cancels both owned sources, which in turn cancels all
 * previously-vended tokens.
 *
 * Cancellation signals
 * --------------------
 * - abort()               cancels _abortOrStepdownSource only
 * - stepdown              cancels both _stepdownSource and _abortOrStepdownSource
 */
class CancelState {
public:
    CancelState(boost::optional<CancellationToken> stepdownToken = boost::none);
    void attachStepdownToken(const CancellationToken& stepdownToken);
    CancellationToken getStepdownToken() const;
    CancellationToken getAbortOrStepdownToken() const;
    bool isSteppingDown() const;
    bool isAbortedOrSteppingDown() const;
    void abort();

private:
    CancellationSource _stepdownSource;
    CancellationSource _abortOrStepdownSource;
};

}  // namespace primary_only_service_helpers
}  // namespace mongo
