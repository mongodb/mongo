/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#pragma once

#include "mongo/util/cancellation.h"
#include "mongo/util/modules.h"

#include <boost/optional/optional.hpp>

namespace MONGO_MOD_PUB mongo {
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
class MONGO_MOD_OPEN CancelState {
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
}  // namespace MONGO_MOD_PUB mongo
