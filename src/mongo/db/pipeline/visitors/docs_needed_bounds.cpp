// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/db/pipeline/visitors/docs_needed_bounds.h"

#include <string_view>

namespace mongo {
namespace docs_needed_bounds {
DocsNeededConstraint parseDocsNeededConstraintFromBSON(const BSONElement& elem) {
    if (elem.isNumber() &&
        (elem.type() == BSONType::numberInt || elem.type() == BSONType::numberLong)) {
        uassert(ErrorCodes::BadValue, "DocsNeededConstraint cannot be NaN.", !elem.isNaN());
        auto val = elem.safeNumberLong();
        uassert(ErrorCodes::BadValue, "DocsNeededConstraint number value must be >= 0.", val >= 0);
        return val;
    } else if (elem.type() == BSONType::string) {
        if (elem.str() == kNeedAllName) {
            return NeedAll();
        } else if (elem.str() == kUnknownName) {
            return Unknown();
        }
    }
    uasserted(ErrorCodes::BadValue,
              str::stream() << "DocsNeededConstraint has to be either of the strings \""
                            << kNeedAllName << "\" or \"" << kUnknownName
                            << "\", or a non-decimal number; "
                            << "found: " << typeName(elem.type()));
}

void serializeDocsNeededConstraint(const DocsNeededConstraint& constraint,
                                   std::string_view fieldName,
                                   BSONObjBuilder* builder) {
    visit(OverloadedVisitor{
              [&](long long val) { builder->append(fieldName, val); },
              [&](NeedAll) { builder->append(fieldName, kNeedAllName); },
              [&](Unknown) { builder->append(fieldName, kUnknownName); },
          },
          constraint);
}

DocsNeededConstraint chooseStrongerMinConstraint(DocsNeededConstraint constraintA,
                                                 DocsNeededConstraint constraintB) {
    return visit(
        OverloadedVisitor{
            [](long long a, long long b) -> DocsNeededConstraint { return std::max(a, b); },
            // For minimum constraints, a discrete value is stronger than Unknown.
            [](Unknown a, long long b) -> DocsNeededConstraint { return b; },
            [](long long a, Unknown b) -> DocsNeededConstraint { return a; },
            [](Unknown a, Unknown b) -> DocsNeededConstraint { return Unknown(); },
            // If we've reached this case, one of the constraints is NeedAll.
            [](auto a, auto b) -> DocsNeededConstraint { return NeedAll(); },
        },
        constraintA,
        constraintB);
}

DocsNeededConstraint chooseStrongerMaxConstraint(DocsNeededConstraint constraintA,
                                                 DocsNeededConstraint constraintB) {
    return visit(
        OverloadedVisitor{
            [](long long a, long long b) -> DocsNeededConstraint { return std::max(a, b); },
            // For maximum constraints, Unknown is stronger than a discrete value.
            [](Unknown a, long long b) -> DocsNeededConstraint { return Unknown(); },
            [](long long a, Unknown b) -> DocsNeededConstraint { return Unknown(); },
            [](Unknown a, Unknown b) -> DocsNeededConstraint { return Unknown(); },
            // If we've reached this case, one of the constraints is NeedAll.
            [](auto a, auto b) -> DocsNeededConstraint { return NeedAll(); },
        },
        constraintA,
        constraintB);
}
}  // namespace docs_needed_bounds
}  // namespace mongo
