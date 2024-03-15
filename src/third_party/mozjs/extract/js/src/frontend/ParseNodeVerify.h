/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_ParseNodeVerify_h
#define frontend_ParseNodeVerify_h

#include "frontend/SyntaxParseHandler.h"  // SyntaxParseHandler::Node

namespace js {

class FrontendContext;
class LifoAlloc;

namespace frontend {

class ParseNode;

// In most builds, examine the given ParseNode and crash if it's not
// well-formed. (In late beta and shipping builds of Firefox, this does
// nothing.)
//
// This returns true on success, and false only if we hit the recursion limit.
// If the ParseNode is actually bad, we crash.

#ifdef DEBUG
[[nodiscard]] extern bool CheckParseTree(FrontendContext* fc,
                                         const LifoAlloc& alloc, ParseNode* pn);
#else
[[nodiscard]] inline bool CheckParseTree(FrontendContext* fc,
                                         const LifoAlloc& alloc,
                                         ParseNode* pn) {
  return true;
}
#endif

[[nodiscard]] inline bool CheckParseTree(FrontendContext* fc,
                                         const LifoAlloc& alloc,
                                         SyntaxParseHandler::Node pn) {
  return true;
}

} /* namespace frontend */
} /* namespace js */

#endif  // frontend_ParseNodeVerify_h
