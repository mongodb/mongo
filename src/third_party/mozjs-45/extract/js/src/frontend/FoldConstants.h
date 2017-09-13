/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_FoldConstants_h
#define frontend_FoldConstants_h

#include "frontend/SyntaxParseHandler.h"

namespace js {
namespace frontend {

// Perform constant folding on the given AST. For example, the program
// `print(2 + 2)` would become `print(4)`.
//
// pnp is the address of a pointer variable that points to the root node of the
// AST. On success, *pnp points to the root node of the new tree, which may be
// the same node (unchanged or modified in place) or a new node.
//
// Usage:
//    pn = parser->statement();
//    if (!pn)
//        return false;
//    if (!FoldConstants(cx, &pn, parser))
//        return false;
bool
FoldConstants(ExclusiveContext* cx, ParseNode** pnp, Parser<FullParseHandler>* parser);

inline bool
FoldConstants(ExclusiveContext* cx, SyntaxParseHandler::Node* pnp,
              Parser<SyntaxParseHandler>* parser)
{
    return true;
}

} /* namespace frontend */
} /* namespace js */

#endif /* frontend_FoldConstants_h */
