// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/query/explain_verbosity_gen.h"
#include "mongo/util/modules.h"

#include <string_view>
#include <type_traits>
#include <utility>

namespace mongo {
using namespace std::literals::string_view_literals;

/**
 * Represents options passed to the explain command (aside from the command which is being explained
 * and its parameters).
 */
class [[MONGO_MOD_PUBLIC]] ExplainOptions {
public:
    /**
     * The various supported verbosity levels for explain. The enum order is not
     * content-significant: explain content decisions go through ExplainPolicy / explainPolicyFor()
     * (see explain_policy.h), and the relational operators on this enum are deleted so no ordinal
     * comparison can compile. Reordering or inserting values here cannot change explain semantics.
     */
    using Verbosity = explain::VerbosityEnum;

    static constexpr std::string_view kVerbosityName = "verbosity"sv;

    /**
     * Returns true if 'verbosity' is one of the "version 3" (V3) explain verbosity modes. When any
     * of these is requested, explain reports "explainVersion: '3'".
     */
    static bool isV3Verbosity(ExplainOptions::Verbosity verbosity);

    /**
     * Converts an explain verbosity to its string representation.
     */
    static std::string_view verbosityString(ExplainOptions::Verbosity verbosity);

    /**
     * Converts 'verbosity' to its corresponding representation as a BSONObj containing explain
     * command parameters.
     */
    static BSONObj toBSON(ExplainOptions::Verbosity verbosity);
};

namespace explain {
// Deleting the relational operators makes every ordinal comparison of a Verbosity (e.g. "v >=
// kExecStats", "v < kExecAllPlans") ill-formed, so explain content decisions must go through
// ExplainPolicy / explainPolicyFor() (see explain_policy.h) instead of the enum's order. As a
// result the enum's order is semantically irrelevant: new verbosity values can never mislead a
// comparison because no such comparison compiles.
//
// These are declared in the enum's own namespace (mongo::explain) so ADL finds them from every
// translation unit that compares two VerbosityEnum values. Equality (== / !=) is intentionally not
// deleted: exact-value checks and hashed containers remain valid.
bool operator<(VerbosityEnum, VerbosityEnum) = delete;
bool operator<=(VerbosityEnum, VerbosityEnum) = delete;
bool operator>(VerbosityEnum, VerbosityEnum) = delete;
bool operator>=(VerbosityEnum, VerbosityEnum) = delete;

// Permanent compile-time regression guard: proves that an ordinal Verbosity comparison is
// ill-formed. If anyone re-enables ordinal comparison later (e.g. by removing the deleted operators
// above), this static_assert fails and the build fails.
//
// This is a SFINAE trait rather than a "requires(...) { a < b; }" expression on purpose: selecting
// a deleted operator inside a requires-expression is a hard error under clang (it is not treated
// as an unsatisfied requirement), whereas the same selection in a decltype/void_t partial
// specialization is a substitution failure.
template <typename T, typename = void>
struct HasOrdinalVerbosityComparison : std::false_type {};
template <typename T>
struct HasOrdinalVerbosityComparison<T,
                                     std::void_t<decltype(std::declval<T>() < std::declval<T>())>>
    : std::true_type {};

static_assert(!HasOrdinalVerbosityComparison<VerbosityEnum>::value,
              "Ordinal Verbosity comparison must not compile - use ExplainPolicy (SERVER-130812)");
}  // namespace explain

}  // namespace mongo
