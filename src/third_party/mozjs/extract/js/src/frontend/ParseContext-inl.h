/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_ParseContext_inl_h
#define frontend_ParseContext_inl_h

#include "frontend/ParseContext.h"

#include "frontend/Parser.h"

namespace js {
namespace frontend {

template <>
inline bool ParseContext::Statement::is<ParseContext::LabelStatement>() const {
  return kind_ == StatementKind::Label;
}

template <>
inline bool ParseContext::Statement::is<ParseContext::ClassStatement>() const {
  return kind_ == StatementKind::Class;
}

template <typename T>
inline T& ParseContext::Statement::as() {
  MOZ_ASSERT(is<T>());
  return static_cast<T&>(*this);
}

inline ParseContext::Scope::BindingIter ParseContext::Scope::bindings(
    ParseContext* pc) {
  // In function scopes with parameter expressions, function special names
  // (like '.this') are declared as vars in the function scope, despite its
  // not being the var scope.
  return BindingIter(*this, pc->varScope_ == this ||
                                pc->functionScope_.ptrOr(nullptr) == this);
}

inline ParseContext::Scope::Scope(ParserBase* parser)
    : Nestable<Scope>(&parser->pc_->innermostScope_),
      declared_(parser->fc_->nameCollectionPool()),
      possibleAnnexBFunctionBoxes_(parser->fc_->nameCollectionPool()),
      id_(parser->usedNames_.nextScopeId()) {}

inline ParseContext::Scope::Scope(FrontendContext* fc, ParseContext* pc,
                                  UsedNameTracker& usedNames)
    : Nestable<Scope>(&pc->innermostScope_),
      declared_(fc->nameCollectionPool()),
      possibleAnnexBFunctionBoxes_(fc->nameCollectionPool()),
      id_(usedNames.nextScopeId()) {}

inline ParseContext::VarScope::VarScope(ParserBase* parser) : Scope(parser) {
  useAsVarScope(parser->pc_);
}

inline ParseContext::VarScope::VarScope(FrontendContext* fc, ParseContext* pc,
                                        UsedNameTracker& usedNames)
    : Scope(fc, pc, usedNames) {
  useAsVarScope(pc);
}

inline JS::Result<Ok, ParseContext::BreakStatementError>
ParseContext::checkBreakStatement(TaggedParserAtomIndex label) {
  // Labeled 'break' statements target the nearest labeled statements (could
  // be any kind) with the same label. Unlabeled 'break' statements target
  // the innermost loop or switch statement.
  if (label) {
    auto hasSameLabel = [&label](ParseContext::LabelStatement* stmt) {
      MOZ_ASSERT(stmt);
      return stmt->label() == label;
    };

    if (!findInnermostStatement<ParseContext::LabelStatement>(hasSameLabel)) {
      return mozilla::Err(ParseContext::BreakStatementError::LabelNotFound);
    }

  } else {
    auto isBreakTarget = [](ParseContext::Statement* stmt) {
      return StatementKindIsUnlabeledBreakTarget(stmt->kind());
    };

    if (!findInnermostStatement(isBreakTarget)) {
      return mozilla::Err(ParseContext::BreakStatementError::ToughBreak);
    }
  }

  return Ok();
}

inline JS::Result<Ok, ParseContext::ContinueStatementError>
ParseContext::checkContinueStatement(TaggedParserAtomIndex label) {
  // Labeled 'continue' statements target the nearest labeled loop
  // statements with the same label. Unlabeled 'continue' statements target
  // the innermost loop statement.
  auto isLoop = [](ParseContext::Statement* stmt) {
    MOZ_ASSERT(stmt);
    return StatementKindIsLoop(stmt->kind());
  };

  if (!label) {
    // Unlabeled statement: we target the innermost loop, so make sure that
    // there is an innermost loop.
    if (!findInnermostStatement(isLoop)) {
      return mozilla::Err(ParseContext::ContinueStatementError::NotInALoop);
    }
    return Ok();
  }

  // Labeled statement: targest the nearest labeled loop with the same label.
  ParseContext::Statement* stmt = innermostStatement();
  bool foundLoop = false;  // True if we have encountered at least one loop.

  for (;;) {
    stmt = ParseContext::Statement::findNearest(stmt, isLoop);
    if (!stmt) {
      return foundLoop
                 ? mozilla::Err(
                       ParseContext::ContinueStatementError::LabelNotFound)
                 : mozilla::Err(
                       ParseContext::ContinueStatementError::NotInALoop);
    }

    foundLoop = true;

    // Is it labeled by our label?
    stmt = stmt->enclosing();
    while (stmt && stmt->is<ParseContext::LabelStatement>()) {
      if (stmt->as<ParseContext::LabelStatement>().label() == label) {
        return Ok();
      }

      stmt = stmt->enclosing();
    }
  }
}

template <typename DeclaredNamePtrT>
inline void RedeclareVar(DeclaredNamePtrT ptr, DeclarationKind kind) {
#ifdef DEBUG
  DeclarationKind declaredKind = ptr->value()->kind();
  MOZ_ASSERT(DeclarationKindIsVar(declaredKind));
#endif

  // Any vars that are redeclared as body-level functions must
  // be recorded as body-level functions.
  //
  // In the case of global and eval scripts, GlobalDeclaration-
  // Instantiation [1] and EvalDeclarationInstantiation [2]
  // check for the declarability of global var and function
  // bindings via CanDeclareVar [3] and CanDeclareGlobal-
  // Function [4]. CanDeclareGlobalFunction is strictly more
  // restrictive than CanDeclareGlobalVar, so record the more
  // restrictive kind. These semantics are implemented in
  // CheckCanDeclareGlobalBinding.
  //
  // VarForAnnexBLexicalFunction declarations are declared when
  // the var scope exits. It is not possible for a var to be
  // previously declared as VarForAnnexBLexicalFunction and
  // checked for redeclaration.
  //
  // [1] ES 15.1.11
  // [2] ES 18.2.1.3
  // [3] ES 8.1.1.4.15
  // [4] ES 8.1.1.4.16
  if (kind == DeclarationKind::BodyLevelFunction) {
    MOZ_ASSERT(declaredKind != DeclarationKind::VarForAnnexBLexicalFunction);
    ptr->value()->alterKind(kind);
  }
}

}  // namespace frontend
}  // namespace js

#endif  // frontend_ParseContext_inl_h
