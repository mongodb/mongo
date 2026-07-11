// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/s/primary_only_service_helpers/phase_transition_progress_gen.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace [[MONGO_MOD_PUBLIC]] mongo {

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
    using ParseFunction = std::function<Phase(std::string_view)>;

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
        return idl::deserialize<PhaseTransitionProgressEnum>(arg, ctx);
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
