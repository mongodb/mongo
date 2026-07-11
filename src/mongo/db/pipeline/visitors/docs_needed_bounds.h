// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/modules.h"
#include "mongo/util/overloaded_visitor.h"

#include <string_view>
#include <variant>

namespace mongo::docs_needed_bounds {
using namespace std::literals::string_view_literals;
static constexpr auto kNeedAllName = "NeedAll"sv;
static constexpr auto kUnknownName = "Unknown"sv;

struct NeedAll {
    // Nothing
};

struct Unknown {
    // Nothing
};

using DocsNeededConstraint = std::variant<long long, NeedAll, Unknown>;

DocsNeededConstraint parseDocsNeededConstraintFromBSON(const BSONElement& elem);
void serializeDocsNeededConstraint(const DocsNeededConstraint& bounds,
                                   std::string_view fieldName,
                                   BSONObjBuilder* builder);

/**
 * Given two DocsNeededConstraints, returns the one that is the stronger constraint, to denote
 * either a minimum or maximum constraint.
 *
 * The strength of a constraint is different for min and max constraints. For example, if you're
 * trying to determine the most restrictive constraints from two pipelines on the same collection,
 * one that produces min and max constraints Unknown and one that produces min and max constraints
 * 100, we can infer that we will need at least 100 (min = 100) but the upper limit is unclear (max
 * = Unknown).
 */
DocsNeededConstraint chooseStrongerMinConstraint(DocsNeededConstraint constraintA,
                                                 DocsNeededConstraint constraintB);
DocsNeededConstraint chooseStrongerMaxConstraint(DocsNeededConstraint constraintA,
                                                 DocsNeededConstraint constraintB);

}  // namespace mongo::docs_needed_bounds
