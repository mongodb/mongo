/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/overloaded_visitor.h"

#include <variant>

namespace mongo::docs_needed_bounds {
static constexpr auto kNeedAllName = "NeedAll"_sd;
static constexpr auto kUnknownName = "Unknown"_sd;

struct NeedAll {
    // Nothing
};

struct Unknown {
    // Nothing
};

using DocsNeededConstraint = std::variant<long long, NeedAll, Unknown>;

DocsNeededConstraint parseDocsNeededConstraintFromBSON(const BSONElement& elem);
void serializeDocsNeededConstraint(const DocsNeededConstraint& bounds,
                                   StringData fieldName,
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
