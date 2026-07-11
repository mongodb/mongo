// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include <clang/AST/AST.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Basic/IdentifierTable.h>
#include <llvm/Support/Casting.h>

namespace mongo {

// Matches declarations whose declaration context is the C++ standard library
// namespace std.

// Note that inline namespaces are silently ignored during the lookup since
// both libstdc++ and libc++ are known to use them for versioning purposes.

// Given:
// \code
//     namespace ns {
//     struct my_type {};
//     using namespace std;
//     }

//     using std::xxxxxx;
//     using ns:my_type;
//     using ns::xxxxxx;
//  \code

// usingDecl(hasAnyUsingShadowDecl(hasTargetDecl(isFromStdNamespace())))
// matches "using std::xxxxxx" and "using ns::xxxxxx".
// */

AST_MATCHER(clang::Decl, isFromStdNamespace) {

    // Get the declaration context of the current node.
    const clang::DeclContext* D = Node.getDeclContext();

    // Iterate through any inline namespaces to get the top-level namespace context.
    while (D->isInlineNamespace())
        D = D->getParent();

    // Check if the top-level context is a namespace and is the translation unit.
    if (!D->isNamespace() || !D->getParent()->isTranslationUnit())
        return false;

    // Retrieve the identifier information of the namespace declaration.
    const clang::IdentifierInfo* Info = llvm::cast<clang::NamespaceDecl>(D)->getIdentifier();

    // Check if the identifier information exists and matches the string "std".
    return (Info && Info->isStr("std"));
}

}  // namespace mongo
