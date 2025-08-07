/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "MongoBannedNamesCheck.h"

#include <iostream>
#include <string>

#include <clang/Lex/Lexer.h>

namespace mongo::tidy {

using namespace clang;
using namespace clang::ast_matchers;

namespace {

enum Action {
    kStdxReplacement,
    kMongoReplacement,
    kBoostReplacement,
    kDoNotUse,
};

enum Namespace {
    kStd,
    kBoost,
};

struct BanInfo {
    std::string produceBanMessage(const llvm::StringRef sourceText) const {
        auto msg = [&]() -> std::string {
            switch (action) {
                case kStdxReplacement:
                    return "Consider using alternatives such as the polyfills from the "
                           "mongo::stdx:: namespace.";
                case kMongoReplacement:
                case kBoostReplacement:
                    return "Consider using " + message + " instead.";
                case kDoNotUse:
                    return "Do not use. " + message;
            }
            return std::string();
        }();

        std::ostringstream err;
        err << "Forbidden use of banned name in " << sourceText.str() << ". " << msg
            << " Use '//  NOLINT' if usage is absolutely necessary."
            << " Be especially careful doing so outside of test code.";
        return err.str();
    }

    bool fromBannedNamespace(const llvm::StringRef sourceText) const {
        for (auto&& ns : namespaces) {
            switch (ns) {
                case kStd:
                    if (sourceText.find("std::" + name.str()) != llvm::StringRef::npos)
                        return true;
                    break;
                case kBoost:
                    if (sourceText.find("boost::" + name.str()) != llvm::StringRef::npos)
                        return true;
                    break;
            }
        }
        return false;
    }

    llvm::StringRef name;
    Action action;
    std::vector<Namespace> namespaces;
    std::string message;
};

std::vector<llvm::StringRef> getNames(const std::vector<BanInfo>& infos) {
    std::vector<llvm::StringRef> names;
    for (auto&& info : infos) {
        names.push_back(info.name);
    }
    return names;
}

// List of base type names from the std and boost namespaces to be checked
const std::vector<BanInfo> baseTypeInfos = {
    {"atomic", kMongoReplacement, {kStd, kBoost}, "mongo::Atomic<T>"},
    {"condition_variable", kStdxReplacement, {kStd, kBoost}},
    {"condition_variable_any", kStdxReplacement, {kStd, kBoost}},
    {"future", kMongoReplacement, {kStd, kBoost}, "mongo::Future"},
    {"launch", kDoNotUse, {kStd}},
    {"optional", kBoostReplacement, {kStd}, "boost::optional"},
    {"packaged_task", kMongoReplacement, {kStd, kBoost}, "mongo::PackagedTask"},
    {"promise", kMongoReplacement, {kStd, kBoost}, "mongo::Promise"},
    {"recursive_mutex",
     kDoNotUse,
     {kStd, kBoost},
     "A recursive mutex is often an indication of a design problem and is prone to deadlocks "
     "because you don't know what code you are calling while holding the lock."},
    {"shared_mutex",
     kMongoReplacement,
     {kStd, kBoost},
     "a type from src/mongo/platform/rwmutex.h or a LockManager lock"},
    {"shared_timed_mutex",
     kMongoReplacement,
     {kStd, kBoost},
     "a type from src/mongo/platform/rwmutex.h or a LockManager lock"},
    {"thread", kStdxReplacement, {kStd, kBoost}},
    {"timed_mutex", kDoNotUse, {kStd, kBoost}, "timed_mutex acquisitions are not interruptible."},
    {"unordered_map", kStdxReplacement, {kStd, kBoost}},
    {"unordered_set", kStdxReplacement, {kStd, kBoost}},
};

// List of base enum names from the std and boost namespaces to be checked
const std::vector<BanInfo> baseEnumInfos = {};

// List of base function names from the std and boost namespaces to be checked
const std::vector<BanInfo> baseFuncInfos = {
    {"async", kDoNotUse, {kStd, kBoost}},
    {"get_terminate", kStdxReplacement, {kStd}},
    {"notify_all_at_thread_exit", kDoNotUse, {kStd, kBoost}},
    {"regex_search", kMongoReplacement, {kStd, kBoost}, "mongo::pcre::Regex"},
    {"set_terminate", kStdxReplacement, {kStd}},
};

// List of base namespace names from the std and boost namespaces to be checked
const std::vector<BanInfo> baseNamespaceInfos = {};

const std::vector<BanInfo> allInfos = [] {
    std::vector<BanInfo> infos;
    auto add = [&](auto& r) {
        infos.insert(infos.end(), r.begin(), r.end());
    };
    add(baseTypeInfos);
    add(baseEnumInfos);
    add(baseFuncInfos);
    add(baseNamespaceInfos);
    return infos;
}();
}  // namespace

void MongoBannedNamesCheck::registerMatchers(ast_matchers::MatchFinder* Finder) {
    // Register AST Matchers to find any use of the banned names.
    Finder->addMatcher(declRefExpr(hasDeclaration(enumConstantDecl(
                                       hasParent(enumDecl(hasAnyName(getNames(baseEnumInfos)))))))
                           .bind("enumConstants"),
                       this);
    // The type matcher matches against DeclaratorDecl nodes, which means return types and
    // references will not be matched here. Matching against only declarations should be
    // sufficient and will not generate extra noise when developers decide to use a banned type.
    Finder->addMatcher(
        declaratorDecl(hasType(namedDecl(hasAnyName(getNames(baseTypeInfos))))).bind("typeNames"),
        this);
    Finder->addMatcher(
        usingDecl(hasAnyUsingShadowDecl(hasTargetDecl(anyOf(hasAnyName(getNames(baseTypeInfos)),
                                                            hasAnyName(getNames(baseFuncInfos))))))
            .bind("usingNames"),
        this);
    Finder->addMatcher(callExpr(callee(expr(hasDescendant(declRefExpr(hasDeclaration(
                                    functionDecl(hasAnyName(getNames(baseFuncInfos)))))))))
                           .bind("functionNames"),
                       this);
    Finder->addMatcher(declRefExpr(hasDeclaration(namedDecl(hasParent(
                                       namespaceDecl(hasAnyName(getNames(baseNamespaceInfos)))))))
                           .bind("namespaceFromRef"),
                       this);
    Finder->addMatcher(expr(hasType(qualType(hasDeclaration(namedDecl(hasParent(
                                namespaceDecl(hasAnyName(getNames(baseNamespaceInfos)))))))))
                           .bind("namespaceFromType"),
                       this);
    Finder->addMatcher(
        declaratorDecl(hasParent(namespaceDecl(hasAnyName(getNames(baseNamespaceInfos)))))
            .bind("namespaceFromDecl"),
        this);
}

void MongoBannedNamesCheck::checkNamespace(const SourceLocation loc,
                                           const llvm::StringRef sourceText) {
    if (!loc.isValid()) {
        return;
    }
    for (auto&& info : allInfos) {
        if (sourceText.contains(info.name.str()) && info.fromBannedNamespace(sourceText)) {
            diag(loc, info.produceBanMessage(sourceText));
            return;
        }
    }
}

// Get text as written in source from a clang node to ensure any alias makes it into the
// returned string.
template <typename Node>
llvm::StringRef getTextFromSource(const Node* node, const clang::ASTContext& Context) {
    const clang::SourceManager& SM = Context.getSourceManager();
    clang::SourceRange Range = node->getSourceRange();
    return Lexer::getSourceText(CharSourceRange::getTokenRange(Range), SM, Context.getLangOpts());
}

void MongoBannedNamesCheck::check(const ast_matchers::MatchFinder::MatchResult& Result) {
    auto chkAs = [&]<typename As>(std::type_identity<As>,
                                  std::initializer_list<std::string> matchNames) {
        for (auto&& name : matchNames) {
            if (auto node = Result.Nodes.getNodeAs<As>(name)) {
                checkNamespace(node->getBeginLoc(), getTextFromSource(node, *Result.Context));
            }
        }
    };
    chkAs(std::type_identity<Expr>{},
          {
              "functionNames",
              "enumConstants",
              "namespaceFromRef",
              "namespaceFromType",
          });
    chkAs(std::type_identity<DeclaratorDecl>{},
          {
              "typeNames",
              "namespaceFromDecl",
          });
    chkAs(std::type_identity<UsingDecl>{},
          {
              "usingNames",
          });
}
}  // namespace mongo::tidy
