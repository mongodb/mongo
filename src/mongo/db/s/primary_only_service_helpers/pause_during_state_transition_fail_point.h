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

#include "mongo/db/s/primary_only_service_helpers/state_transition_progress_gen.h"

namespace mongo {

namespace primary_only_service_helpers {

/**
 * Helper class to make it easier to implementing pausing during state transitions for
 * PrimaryOnlyServices in a way that's easy to control programmatically. The State template
 * parameter should be the state enum type for your PrimaryOnlyService. Failpoints registered with
 * this class can be used with the following data:
 * {
 *     progress: <"before", "partial", or "after">,
 *     state: <string of associated State as determined by the supplied ParseFunction>
 * }
 * Then, when calling evaluate(progress, state), each of the supplied failpoints will pause if its
 * data matches the arguments.
 */
template <typename State>
class PauseDuringStateTransitionFailPoint {
public:
    using FailPoints = std::vector<std::reference_wrapper<FailPoint>>;
    using ParseFunction = std::function<State(StringData)>;

    PauseDuringStateTransitionFailPoint(FailPoint& failpoint, ParseFunction parse)
        : PauseDuringStateTransitionFailPoint{FailPoints{failpoint}, std::move(parse)} {}

    /**
     * All failpoints in the failpoints list will use the same logic for pausing. A single failpoint
     * can't be active multiple times with different arguments, but setting up more complex
     * scenarios sometimes requires multiple failpoints.
     */
    PauseDuringStateTransitionFailPoint(FailPoints failpoints, ParseFunction parse)
        : _failpoints{std::move(failpoints)}, _parse{std::move(parse)} {}

    /**
     * Users of this class should call evaluate with the associated StateTransitionProgress value
     * and new state at appropriate times (see comments on StateTransitionProgress) during their
     * state transition logic.
     */
    void evaluate(StateTransitionProgressEnum progress, State newState) {
        for (auto& failpoint : _failpoints) {
            _evaluatePauseDuringStateTransitionFailpoint(progress, newState, failpoint);
        }
    }

private:
    boost::optional<StateTransitionProgressEnum> _readProgressArgument(const BSONObj& data) {
        auto arg = data.getStringField("progress");
        IDLParserContext ctx("PauseDuringStateTransitionFailPoint::readProgressArgument");
        return StateTransitionProgress_parse(ctx, arg);
    }

    boost::optional<State> _readStateArgument(const BSONObj& data) {
        try {
            auto arg = data.getStringField("state");
            return _parse(arg);
        } catch (...) {
            return boost::none;
        }
    }

    void _evaluatePauseDuringStateTransitionFailpoint(StateTransitionProgressEnum progress,
                                                      State newState,
                                                      FailPoint& failpoint) {
        failpoint.executeIf([&](const auto& data) { failpoint.pauseWhileSet(); },
                            [&](const auto& data) {
                                auto desiredProgress = _readProgressArgument(data);
                                auto desiredState = _readStateArgument(data);
                                if (!desiredProgress.has_value() || !desiredState.has_value()) {
                                    return false;
                                }
                                return *desiredProgress == progress && *desiredState == newState;
                            });
    }

    FailPoints _failpoints;
    ParseFunction _parse;
};
}  // namespace primary_only_service_helpers

}  // namespace mongo
