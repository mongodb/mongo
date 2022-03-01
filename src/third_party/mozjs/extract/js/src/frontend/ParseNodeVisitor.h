/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_ParseNodeVisitor_h
#define frontend_ParseNodeVisitor_h

#include "mozilla/Assertions.h"

#include "jsfriendapi.h"

#include "frontend/ParseNode.h"
#include "js/friend/StackLimits.h"  // js::AutoCheckRecursionLimit

namespace js {
namespace frontend {

/**
 * Utility class for walking a JS AST.
 *
 * Simple usage:
 *
 *     class HowTrueVisitor : public ParseNodeVisitor<HowTrueVisitor> {
 *     public:
 *       bool visitTrueExpr(BooleanLiteral* pn) {
 *         std::cout << "How true.\n";
 *         return true;
 *       }
 *       bool visitClassDecl(ClassNode* pn) {
 *         // The base-class implementation of each visit method
 *         // simply visits the node's children. So the subclass
 *         // gets to decide whether to descend into a subtree
 *         // and can do things either before or after:
 *         std::cout << "How classy.\n";
 *         return ParseNodeVisitor::visitClassDecl(pn);
 *       }
 *     };
 *
 *     HowTrueVisitor v;
 *     v.visit(programRootNode);  // walks the entire tree
 *
 * A ParseNodeVisitor can modify nodes, but it can't replace the current node
 * with a different one; for that, use a RewritingParseNodeVisitor.
 *
 * Note that the Curiously Recurring Template Pattern is used for performance,
 * as it eliminates the need for virtual method calls. Some rough testing shows
 * about a 12% speedup in the FoldConstants.cpp pass.
 * https://en.wikipedia.org/wiki/Curiously_recurring_template_pattern
 */
template <typename Derived>
class ParseNodeVisitor {
 public:
  JSContext* cx_;

  explicit ParseNodeVisitor(JSContext* cx) : cx_(cx) {}

  [[nodiscard]] bool visit(ParseNode* pn) {
    AutoCheckRecursionLimit recursion(cx_);
    if (!recursion.check(cx_)) {
      return false;
    }

    switch (pn->getKind()) {
#define VISIT_CASE(KIND, TYPE) \
  case ParseNodeKind::KIND:    \
    return static_cast<Derived*>(this)->visit##KIND(&pn->as<TYPE>());
      FOR_EACH_PARSE_NODE_KIND(VISIT_CASE)
#undef VISIT_CASE
      default:
        MOZ_CRASH("invalid node kind");
    }
  }

  // using static_cast<Derived*> here allows plain visit() to be overridden.
#define VISIT_METHOD(KIND, TYPE)                          \
  [[nodiscard]] bool visit##KIND(TYPE* pn) { /* NOLINT */ \
    return pn->accept(*static_cast<Derived*>(this));      \
  }
  FOR_EACH_PARSE_NODE_KIND(VISIT_METHOD)
#undef VISIT_METHOD
};

/*
 * Utility class for walking a JS AST that allows for replacing nodes.
 *
 * The API is the same as ParseNodeVisitor, except that visit methods take an
 * argument of type `ParseNode*&`, a reference to the field in the parent node
 * that points down to the node being visited. Methods can replace the current
 * node by assigning to this reference.
 *
 * All visit methods take a `ParseNode*&` rather than more specific types like
 * `BinaryNode*&`, to allow replacing the current node with one of a different
 * type. Constant folding makes use of this.
 */
template <typename Derived>
class RewritingParseNodeVisitor {
 public:
  JSContext* cx_;

  explicit RewritingParseNodeVisitor(JSContext* cx) : cx_(cx) {}

  [[nodiscard]] bool visit(ParseNode*& pn) {
    AutoCheckRecursionLimit recursion(cx_);
    if (!recursion.check(cx_)) {
      return false;
    }

    switch (pn->getKind()) {
#define VISIT_CASE(KIND, _type) \
  case ParseNodeKind::KIND:     \
    return static_cast<Derived*>(this)->visit##KIND(pn);
      FOR_EACH_PARSE_NODE_KIND(VISIT_CASE)
#undef VISIT_CASE
      default:
        MOZ_CRASH("invalid node kind");
    }
  }

  // using static_cast<Derived*> here allows plain visit() to be overridden.
#define VISIT_METHOD(KIND, TYPE)                                 \
  [[nodiscard]] bool visit##KIND(ParseNode*& pn) {               \
    MOZ_ASSERT(pn->is<TYPE>(),                                   \
               "Node of kind " #KIND " was not of type " #TYPE); \
    return pn->as<TYPE>().accept(*static_cast<Derived*>(this));  \
  }
  FOR_EACH_PARSE_NODE_KIND(VISIT_METHOD)
#undef VISIT_METHOD
};

}  // namespace frontend
}  // namespace js

#endif  // frontend_ParseNodeVisitor_h
