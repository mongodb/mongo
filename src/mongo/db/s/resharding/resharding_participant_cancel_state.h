/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/s/primary_only_service_helpers/cancel_state.h"
#include "mongo/util/cancellation.h"

#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * Wraps CancelState to add change streams monitor (CSM) cancellation support. The CSM runs in
 * parallel with the main resharding state machine and can be cancelled independently.
 *
 * Cancellation Hierarchy:
 *   stepdownToken (replica set primary stepped down)
 *     └─> abortOrStepdownToken (user/coordinator requested abort OR stepdown)
 *           └─> changeStreamsMonitorToken (monitor failure OR abort OR stepdown)
 */
class ReshardingParticipantCancelState {
public:
    ReshardingParticipantCancelState(boost::optional<CancellationToken> stepdownToken = boost::none)
        : _cancelState(stepdownToken) {}

    void attachStepdownToken(const CancellationToken& stepdownToken) {
        _cancelState.attachStepdownToken(stepdownToken);
    }

    CancellationToken getStepdownToken() const {
        return _cancelState.getStepdownToken();
    }

    CancellationToken getAbortOrStepdownToken() const {
        return _cancelState.getAbortOrStepdownToken();
    }

    bool isSteppingDown() const {
        return _cancelState.isSteppingDown();
    }

    bool isAbortedOrSteppingDown() const {
        return _cancelState.isAbortedOrSteppingDown();
    }

    void abort() {
        _cancelState.abort();
    }

    void createChangeStreamsMonitorAbortSource() {
        if (_changeStreamsMonitorAbortSource) {
            return;
        }
        _changeStreamsMonitorAbortSource =
            CancellationSource(_cancelState.getAbortOrStepdownToken());
    }

    void cancelChangeStreamsMonitor() {
        tassert(10885201,
                "No change streams monitor abort source was created",
                _changeStreamsMonitorAbortSource);
        _changeStreamsMonitorAbortSource->cancel();
    }

    CancellationToken getEffectiveCancellationToken() const {
        if (_changeStreamsMonitorAbortSource) {
            return _changeStreamsMonitorAbortSource->token();
        }
        return _cancelState.getAbortOrStepdownToken();
    }

    bool isEffectiveTokenCancelled() const {
        return getEffectiveCancellationToken().isCanceled();
    }

private:
    primary_only_service_helpers::CancelState _cancelState;
    boost::optional<CancellationSource> _changeStreamsMonitorAbortSource;
};

}  // namespace mongo
