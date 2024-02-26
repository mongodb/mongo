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

#include "mongo/db/s/primary_only_service_helpers/phase_transition_progress_gen.h"

namespace mongo {

namespace primary_only_service_helpers {

/**
 * Helper class to make it easier to implementing pausing during phase transitions for
 * PrimaryOnlyServices in a way that's easy to control programmatically. The Phase template
 * parameter should be the phase enum type for your PrimaryOnlyService. Failpoints registered with
 * this class can be used with the following data:
 * {
 *     progress: <"before", "partial", or "after">,
 *     phase: <string of associated Phase as determined by the supplied ParseFunction>
 * }
 * Then, when calling evaluate(progress, phase), each of the supplied failpoints will pause if its
 * data matches the arguments.
 */
template <typename Phase>
class PauseDuringPhaseTransitionFailPoint {
public:
    using FailPoints = std::vector<std::reference_wrapper<FailPoint>>;
    using ParseFunction = std::function<Phase(StringData)>;

    PauseDuringPhaseTransitionFailPoint(FailPoint& failpoint, ParseFunction parse)
        : PauseDuringPhaseTransitionFailPoint{FailPoints{failpoint}, std::move(parse)} {}

    /**
     * All failpoints in the failpoints list will use the same logic for pausing. A single failpoint
     * can't be active multiple times with different arguments, but setting up more complex
     * scenarios sometimes requires multiple failpoints.
     */
    PauseDuringPhaseTransitionFailPoint(FailPoints failpoints, ParseFunction parse)
        : _failpoints{std::move(failpoints)}, _parse{std::move(parse)} {}

    /**
     * Users of this class should call evaluate with the associated PhaseTransitionProgress value
     * and new phase at appropriate times (see comments on PhaseTransitionProgress) during their
     * phase transition logic.
     */
    void evaluate(PhaseTransitionProgressEnum progress, Phase newPhase) {
        for (auto& failpoint : _failpoints) {
            _evaluatePauseDuringPhaseTransitionFailpoint(progress, newPhase, failpoint);
        }
    }

private:
    boost::optional<PhaseTransitionProgressEnum> _readProgressArgument(const BSONObj& data) {
        auto arg = data.getStringField("progress");
        IDLParserContext ctx("PauseDuringPhaseTransitionFailPoint::readProgressArgument");
        return PhaseTransitionProgress_parse(ctx, arg);
    }

    boost::optional<Phase> _readPhaseArgument(const BSONObj& data) {
        try {
            auto arg = data.getStringField("phase");
            return _parse(arg);
        } catch (...) {
            return boost::none;
        }
    }

    void _evaluatePauseDuringPhaseTransitionFailpoint(PhaseTransitionProgressEnum progress,
                                                      Phase newPhase,
                                                      FailPoint& failpoint) {
        failpoint.executeIf([&](const auto& data) { failpoint.pauseWhileSet(); },
                            [&](const auto& data) {
                                auto desiredProgress = _readProgressArgument(data);
                                auto desiredPhase = _readPhaseArgument(data);
                                if (!desiredProgress.has_value() || !desiredPhase.has_value()) {
                                    return false;
                                }
                                return *desiredProgress == progress && *desiredPhase == newPhase;
                            });
    }

    FailPoints _failpoints;
    ParseFunction _parse;
};
}  // namespace primary_only_service_helpers

}  // namespace mongo
