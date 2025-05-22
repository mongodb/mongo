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

#include "MongoPolyFillCheck.h"

#include <array>

#include <clang/Lex/Lexer.h>

namespace mongo::tidy {

using namespace clang;
using namespace clang::ast_matchers;

namespace {

// List of base polyfill type names from the std and boost namespaces to be checked
constexpr std::initializer_list<llvm::StringRef> basePolyfillTypeNames = {
    "condition_variable",
    "condition_variable_any",
    "cv_status",
    "future",
    "future_status",
    "launch",
    "packaged_task",
    "promise",
    "recursive_mutex",
    "shared_mutex",
    "shared_timed_mutex",
    "thread",
    "timed_mutex",
    "unordered_map",
    "unordered_multimap",
    "unordered_multiset",
    "unordered_set",
};

// List of base polyfill enum names from the std and boost namespaces to be checked
constexpr std::initializer_list<llvm::StringRef> basePolyfillEnumNames = {
    "cv_status",
    "future_status",
    "launch",
};

// List of base polyfill function names from the std and boost namespaces to be checked
constexpr std::initializer_list<llvm::StringRef> basePolyfillFuncNames = {
    "async",
    "get_terminate",
    "notify_all_at_thread_exit",
    "set_terminate",
};

// List of base polyfill namespace names from the std and boost namespaces to be checked
constexpr std::initializer_list<llvm::StringRef> basePolyfillNamespaces = {
    "this_thread",
    "chrono",
};
}  // namespace

// Generate a list of fully qualified polyfill names by prefixing each name
// in the input list with 'std::' and 'boost::'
std::vector<std::string> generateQualifiedPolyfillNames(
    const std::vector<llvm::StringRef>& bannedNames) {
    std::vector<std::string> fullyBannedNames;
    for (auto&& name : bannedNames) {
        fullyBannedNames.push_back("std::" + name.str());
        fullyBannedNames.push_back("boost::" + name.str());
    }
    return fullyBannedNames;
}

MongoPolyFillCheck::MongoPolyFillCheck(StringRef Name, clang::tidy::ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {
    std::vector<llvm::StringRef> basePolyfillNames;
    basePolyfillNames.insert(
        basePolyfillNames.end(), basePolyfillTypeNames.begin(), basePolyfillTypeNames.end());
    basePolyfillNames.insert(
        basePolyfillNames.end(), basePolyfillEnumNames.begin(), basePolyfillEnumNames.end());
    basePolyfillNames.insert(
        basePolyfillNames.end(), basePolyfillFuncNames.begin(), basePolyfillFuncNames.end());
    basePolyfillNames.insert(
        basePolyfillNames.end(), basePolyfillNamespaces.begin(), basePolyfillNamespaces.end());
    // Generate a list of fully polyfill names
    fullyQualifiedPolyfillNames = generateQualifiedPolyfillNames(basePolyfillNames);
}


void MongoPolyFillCheck::registerMatchers(ast_matchers::MatchFinder* Finder) {
    // Register AST Matchers to find any use of the banned names.
    Finder->addMatcher(declRefExpr(hasDeclaration(enumConstantDecl(
                                       hasParent(enumDecl(hasAnyName(basePolyfillEnumNames))))))
                           .bind("bannedEnumConstants"),
                       this);
    Finder->addMatcher(declaratorDecl(hasType(namedDecl(hasAnyName(basePolyfillTypeNames))))
                           .bind("bannedTypeNames"),
                       this);
    Finder->addMatcher(callExpr(callee(expr(hasDescendant(declRefExpr(hasDeclaration(
                                    functionDecl(hasAnyName(basePolyfillFuncNames))))))))
                           .bind("bannedFunctionNames"),
                       this);
    Finder->addMatcher(declRefExpr(hasDeclaration(namedDecl(hasParent(
                                       namespaceDecl(hasAnyName(basePolyfillNamespaces))))))
                           .bind("bannedNamespaceFromRef"),
                       this);
    Finder->addMatcher(expr(hasType(qualType(hasDeclaration(namedDecl(
                                hasParent(namespaceDecl(hasAnyName(basePolyfillNamespaces))))))))
                           .bind("bannedNamespaceFromType"),
                       this);
    Finder->addMatcher(declaratorDecl(hasParent(namespaceDecl(hasAnyName(basePolyfillNamespaces))))
                           .bind("bannedNamespaceFromDecl"),
                       this);
}

void MongoPolyFillCheck::checkBannedName(const SourceLocation loc, const llvm::StringRef name) {
    // we catch this_thread but not this_thread::at_thread_exit
    if (name.find("this_thread::at_thread_exit") != std::string::npos)
        return;

    // Check if the type string starts with 'std' or 'boost' and contains a banned name.
    for (auto&& polyfillName : fullyQualifiedPolyfillNames) {
        if ((name.starts_with("std::") || name.starts_with("boost::")) &&
            name.find(polyfillName) != std::string::npos) {
            if (loc.isValid())
                diag(loc,
                     "Illegal use of banned name from std::/boost:: for %0. Consider using "
                     "alternatives such as the polyfills from the mongo::stdx:: namespace.")
                    << name;
        }
    }
}

// Get full name as written in source from a clang node to ensure any alias makes it into the
// returned string.
template <typename Node>
llvm::StringRef getFullName(const Node* node, const clang::ASTContext& Context) {
    const clang::SourceManager& SM = Context.getSourceManager();
    clang::SourceRange Range = node->getSourceRange();
    return Lexer::getSourceText(CharSourceRange::getTokenRange(Range), SM, Context.getLangOpts());
}

void MongoPolyFillCheck::check(const ast_matchers::MatchFinder::MatchResult& Result) {
    for (auto&& matcher : {"bannedFunctionNames",
                           "bannedEnumConstants",
                           "bannedNamespaceFromRef",
                           "bannedNamespaceFromType"}) {
        if (const auto* matched = Result.Nodes.getNodeAs<Expr>(matcher)) {
            auto name = getFullName(matched, *Result.Context);
            checkBannedName(matched->getBeginLoc(), std::move(name));
        }
    }

    // DeclaratorDecl inherits from Decl instead of Expr, so it's extracted separately.
    for (auto&& matcher : {"bannedTypeNames", "bannedNamespaceFromDecl"}) {
        if (const auto* matched = Result.Nodes.getNodeAs<DeclaratorDecl>(matcher)) {
            auto name = getFullName(matched, *Result.Context);
            checkBannedName(matched->getBeginLoc(), std::move(name));
        }
    }
}
}  // namespace mongo::tidy
