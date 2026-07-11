// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
