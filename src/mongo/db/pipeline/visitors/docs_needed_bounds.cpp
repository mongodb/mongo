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
#include "mongo/db/pipeline/visitors/docs_needed_bounds.h"

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
                                   StringData fieldName,
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
