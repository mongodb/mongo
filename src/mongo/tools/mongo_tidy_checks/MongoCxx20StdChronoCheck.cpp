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

#include "MongoCxx20StdChronoCheck.h"

namespace mongo::tidy {

using namespace clang;
using namespace clang::ast_matchers;

MongoCxx20StdChronoCheck::MongoCxx20StdChronoCheck(StringRef Name,
                                                   clang::tidy::ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void MongoCxx20StdChronoCheck::registerMatchers(ast_matchers::MatchFinder* Finder) {
    auto chronoClassDeclMatcher = cxxRecordDecl(hasAnyName("utc_clock",
                                                           "tai_clock",
                                                           "gps_clock",
                                                           "local_t",
                                                           "last_spec",
                                                           "day",
                                                           "month",
                                                           "year",
                                                           "weekday",
                                                           "weekday_indexed",
                                                           "weekday_last",
                                                           "month_day",
                                                           "month_day_last",
                                                           "month_weekday",
                                                           "month_weekday_last",
                                                           "year_month",
                                                           "year_month_day",
                                                           "year_month_day_last",
                                                           "year_month_weekday",
                                                           "year_month_weekday_last",
                                                           "tzdb",
                                                           "tzdb_list",
                                                           "time_zone",
                                                           "sys_info",
                                                           "local_info",
                                                           "zoned_time",
                                                           "leap_second",
                                                           "leap_second_info",
                                                           "time_zone_link",
                                                           "nonexistent_local_time",
                                                           "ambiguous_local_time"),
                                                hasDeclContext(namedDecl(hasName("chrono"))));
    Finder->addMatcher(
        loc(recordType(hasDeclaration(chronoClassDeclMatcher))).bind("loc_cxx20_chrono"), this);
}

void MongoCxx20StdChronoCheck::check(const ast_matchers::MatchFinder::MatchResult& Result) {
    const TypeLoc* loc_cxx20_chrono = Result.Nodes.getNodeAs<TypeLoc>("loc_cxx20_chrono");
    if (loc_cxx20_chrono) {
        diag(loc_cxx20_chrono->getBeginLoc(), "Illegal use of prohibited type %0.")
            << loc_cxx20_chrono->getType();
    }
}

}  // namespace mongo::tidy
