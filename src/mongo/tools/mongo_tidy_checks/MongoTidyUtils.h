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
