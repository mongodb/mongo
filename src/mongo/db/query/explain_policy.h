// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/explain_options.h"

#include <cstdint>

namespace mongo {

/**
 * Independent, combinable content capabilities that make up an explain policy. Each flag
 * identifies a specific content that should be present in the generated explain.
 *
 * The bit values are stable identifiers, NOT an order: no explain content decision may depend on
 * the relative magnitude of these values. There is one flag per content decision.
 * See ExplainPolicy and explainPolicyFor() below.
 */
enum class ExplainSettings : uint32_t {
    kNone = 0,
    kPlannerInfo = 1u << 0,    // queryPlanner content: the winning plan (present for every explain)
    kRejectedPlans = 1u << 1,  // queryPlanner content: the candidate / rejected plans
    kExecStats = 1u << 2,      // winning-plan execution statistics
    kAllPlansExecStats = 1u << 3,  // per-candidate all-plans multiplanning statistics
    kBytecode = 1u << 4,           // SBE virtual-machine bytecode of the winning plan
};

constexpr ExplainSettings operator|(ExplainSettings a, ExplainSettings b) {
    return static_cast<ExplainSettings>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
constexpr ExplainSettings operator&(ExplainSettings a, ExplainSettings b) {
    return static_cast<ExplainSettings>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}
constexpr ExplainSettings operator~(ExplainSettings a) {
    return static_cast<ExplainSettings>(~static_cast<uint32_t>(a));
}

/**
 * A type that answers "should this content be present?" questions by name, replacing the old
 * ordinal comparisons of ExplainOptions::Verbosity. Construct one from a verbosity via
 * explainPolicyFor(); content-generation code then calls only the named predicates below and never
 * inspects the verbosity's enum order.
 *
 * Modeled as a set of independent flags (a bitmask) rather than an ordinal level so that a future
 * project can express arbitrary content subsets by layering per-setting deltas onto a verbosity's
 * baseline (see with()/without()/mergedWith()).
 */
class ExplainPolicy {
public:
    constexpr ExplainPolicy() = default;
    constexpr explicit ExplainPolicy(ExplainSettings bits) : _bits(bits) {}

    // The only way explain code asks "should X be present?". One predicate per flag.
    // Whether the "queryPlanner" section (the winning plan, and rejected plans) should be present.
    constexpr bool hasPlannerInfo() const {
        return has(ExplainSettings::kPlannerInfo);
    }
    constexpr bool hasRejectedPlans() const {
        return has(ExplainSettings::kRejectedPlans);
    }
    constexpr bool hasExecStats() const {
        return has(ExplainSettings::kExecStats);
    }
    constexpr bool hasAllPlansStats() const {
        return has(ExplainSettings::kAllPlansExecStats);
    }
    // Whether the winning plan's SBE virtual-machine bytecode should be present.
    constexpr bool hasByteCode() const {
        return has(ExplainSettings::kBytecode);
    }

    // Build arbitrary subsets from a baseline plus deltas.
    constexpr ExplainPolicy with(ExplainSettings c) const {
        return ExplainPolicy(_bits | c);
    }
    constexpr ExplainPolicy without(ExplainSettings c) const {
        return ExplainPolicy(_bits & ~c);
    }
    constexpr ExplainPolicy mergedWith(ExplainPolicy o) const {
        return ExplainPolicy(_bits | o._bits);
    }

    // Returns true iff every bit in 'c' is set. kNone yields false (there is no "none" content to
    // be present).
    constexpr bool has(ExplainSettings c) const {
        const auto bits = static_cast<uint32_t>(c);
        return bits != 0 && (static_cast<uint32_t>(_bits) & bits) == bits;
    }

    friend constexpr bool operator==(ExplainPolicy, ExplainPolicy) = default;

private:
    ExplainSettings _bits{ExplainSettings::kNone};
};

/**
 * The single place that maps a verbosity to its baseline content policy. This is the sole seam
 * through which "what content does this verbosity imply?" is answered; a future explain-settings
 * project can layer deltas onto the returned baseline.
 */
ExplainPolicy explainPolicyFor(ExplainOptions::Verbosity v);

/**
 * Maps a requested (possibly V3) verbosity to the nearest legacy verbosity. Used only by the
 * interim V3 explain hooks, which still delegate to the legacy generators; the mapping matches the
 * legacy verbosities the pre-refactor dispatch passed explicitly (planSummary / plannerChoice ->
 * queryPlanner, plannerStats -> execAllPlans, execStatsV3 -> execStats), and legacy verbosities map
 * to themselves.
 *
 * TODO SERVER-130529 (find path) / SERVER-130810 (aggregation path): remove this once the real V3
 * output format is produced directly instead of via legacy delegation.
 */
ExplainOptions::Verbosity mapV3ToLegacyVerbosity(ExplainOptions::Verbosity v);

}  // namespace mongo
