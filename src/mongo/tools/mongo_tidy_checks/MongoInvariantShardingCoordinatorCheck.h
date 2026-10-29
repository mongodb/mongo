// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include <map>
#include <string_view>
#include <vector>

#include <clang-tidy/ClangTidy.h>
#include <clang-tidy/ClangTidyCheck.h>

namespace mongo::tidy {

class InvariantShardingCoordinatorCheck : public clang::tidy::ClangTidyCheck {
public:
    InvariantShardingCoordinatorCheck(clang::StringRef Name,
                                      clang::tidy::ClangTidyContext* Context);
    void registerMatchers(clang::ast_matchers::MatchFinder* Finder) override;
    void check(const clang::ast_matchers::MatchFinder::MatchResult& Result) override;
    void onEndOfTranslationUnit() override;

private:
    struct FileContext {
        bool hasCoordinatorMethod = false;
        std::vector<const clang::CallExpr*> invariantCalls;
    };

    std::map<std::string_view, FileContext> files;
};

}  // namespace mongo::tidy
