/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "frontend/ParseNode.h"

#include "mozilla/FloatingPoint.h"
#include "mozilla/Try.h"  // MOZ_TRY*

#include "jsnum.h"

#include "frontend/CompilationStencil.h"  // ExtensibleCompilationStencil
#include "frontend/FullParseHandler.h"
#include "frontend/ParseContext.h"
#include "frontend/Parser.h"      // ParserBase
#include "frontend/ParserAtom.h"  // ParserAtomsTable, TaggedParserAtomIndex
#include "frontend/SharedContext.h"
#include "js/Printer.h"
#include "vm/Scope.h"  // GetScopeDataTrailingNames

using namespace js;
using namespace js::frontend;

#ifdef DEBUG
void ListNode::checkConsistency() const {
  ParseNode* const* tailNode;
  uint32_t actualCount = 0;
  if (const ParseNode* last = head()) {
    const ParseNode* pn = last;
    while (pn) {
      last = pn;
      pn = pn->pn_next;
      actualCount++;
    }

    tailNode = &last->pn_next;
  } else {
    tailNode = &head_;
  }
  MOZ_ASSERT(tail() == tailNode);
  MOZ_ASSERT(count() == actualCount);
}
#endif

/*
 * Allocate a ParseNode from parser's node freelist or, failing that, from
 * cx's temporary arena.
 */
void* ParseNodeAllocator::allocNode(size_t size) {
  LifoAlloc::AutoFallibleScope fallibleAllocator(&alloc);
  void* p = alloc.alloc(size);
  if (!p) {
    ReportOutOfMemory(fc);
  }
  return p;
}

ParseNodeResult ParseNode::appendOrCreateList(ParseNodeKind kind,
                                              ParseNode* left, ParseNode* right,
                                              FullParseHandler* handler,
                                              ParseContext* pc) {
  // The asm.js specification is written in ECMAScript grammar terms that
  // specify *only* a binary tree.  It's a royal pain to implement the asm.js
  // spec to act upon n-ary lists as created below.  So for asm.js, form a
  // binary tree of lists exactly as ECMAScript would by skipping the
  // following optimization.
  if (!pc->useAsmOrInsideUseAsm()) {
    // Left-associative trees of a given operator (e.g. |a + b + c|) are
    // binary trees in the spec: (+ (+ a b) c) in Lisp terms.  Recursively
    // processing such a tree, exactly implemented that way, would blow the
    // the stack.  We use a list node that uses O(1) stack to represent
    // such operations: (+ a b c).
    //
    // (**) is right-associative; per spec |a ** b ** c| parses as
    // (** a (** b c)). But we treat this the same way, creating a list
    // node: (** a b c). All consumers must understand that this must be
    // processed with a right fold, whereas the list (+ a b c) must be
    // processed with a left fold because (+) is left-associative.
    //
    if (left->isKind(kind) &&
        (kind == ParseNodeKind::PowExpr ? !left->isInParens()
                                        : left->isBinaryOperation())) {
      ListNode* list = &left->as<ListNode>();

      list->append(right);
      list->pn_pos.end = right->pn_pos.end;

      return list;
    }
  }

  ListNode* list;
  MOZ_TRY_VAR(list, handler->newResult<ListNode>(kind, left));

  list->append(right);
  return list;
}

const ParseNode::TypeCode ParseNode::typeCodeTable[] = {
#define TYPE_CODE(_name, type) type::classTypeCode(),
    FOR_EACH_PARSE_NODE_KIND(TYPE_CODE)
#undef TYPE_CODE
};

#ifdef DEBUG

const size_t ParseNode::sizeTable[] = {
#  define NODE_SIZE(_name, type) sizeof(type),
    FOR_EACH_PARSE_NODE_KIND(NODE_SIZE)
#  undef NODE_SIZE
};

static const char* const parseNodeNames[] = {
#  define STRINGIFY(name, _type) #name,
    FOR_EACH_PARSE_NODE_KIND(STRINGIFY)
#  undef STRINGIFY
};

static void DumpParseTree(const ParserAtomsTable* parserAtoms, ParseNode* pn,
                          GenericPrinter& out, int indent) {
  if (pn == nullptr) {
    out.put("#NULL");
  } else {
    pn->dump(parserAtoms, out, indent);
  }
}

void frontend::DumpParseTree(ParserBase* parser, ParseNode* pn,
                             GenericPrinter& out, int indent) {
  ParserAtomsTable* parserAtoms = parser ? &parser->parserAtoms() : nullptr;
  ::DumpParseTree(parserAtoms, pn, out, indent);
}

static void IndentNewLine(GenericPrinter& out, int indent) {
  out.putChar('\n');
  for (int i = 0; i < indent; ++i) {
    out.putChar(' ');
  }
}

void ParseNode::dump() { dump(nullptr); }

void ParseNode::dump(const ParserAtomsTable* parserAtoms) {
  js::Fprinter out(stderr);
  dump(parserAtoms, out);
}

void ParseNode::dump(const ParserAtomsTable* parserAtoms, GenericPrinter& out) {
  dump(parserAtoms, out, 0);
  out.putChar('\n');
}

void ParseNode::dump(const ParserAtomsTable* parserAtoms, GenericPrinter& out,
                     int indent) {
  switch (getKind()) {
#  define DUMP(K, T)                              \
    case ParseNodeKind::K:                        \
      as<T>().dumpImpl(parserAtoms, out, indent); \
      break;
    FOR_EACH_PARSE_NODE_KIND(DUMP)
#  undef DUMP
    default:
      out.printf("#<BAD NODE %p, kind=%u>", (void*)this, unsigned(getKind()));
  }
}

void NullaryNode::dumpImpl(const ParserAtomsTable* parserAtoms,
                           GenericPrinter& out, int indent) {
  switch (getKind()) {
    case ParseNodeKind::TrueExpr:
      out.put("#true");
      break;
    case ParseNodeKind::FalseExpr:
      out.put("#false");
      break;
    case ParseNodeKind::NullExpr:
      out.put("#null");
      break;
    case ParseNodeKind::RawUndefinedExpr:
      out.put("#undefined");
      break;

    default:
      out.printf("(%s)", parseNodeNames[getKindAsIndex()]);
  }
}

void NumericLiteral::dumpImpl(const ParserAtomsTable* parserAtoms,
                              GenericPrinter& out, int indent) {
  ToCStringBuf cbuf;
  const char* cstr = NumberToCString(&cbuf, value());
  MOZ_ASSERT(cstr);
  if (!std::isfinite(value())) {
    out.put("#");
  }
  out.printf("%s", cstr);
}

void BigIntLiteral::dumpImpl(const ParserAtomsTable* parserAtoms,
                             GenericPrinter& out, int indent) {
  out.printf("(%s)", parseNodeNames[getKindAsIndex()]);
}

void RegExpLiteral::dumpImpl(const ParserAtomsTable* parserAtoms,
                             GenericPrinter& out, int indent) {
  out.printf("(%s)", parseNodeNames[getKindAsIndex()]);
}

static void DumpCharsNoNewline(const ParserAtomsTable* parserAtoms,
                               TaggedParserAtomIndex index,
                               GenericPrinter& out) {
  out.put("\"");
  if (parserAtoms) {
    parserAtoms->dumpCharsNoQuote(out, index);
  } else {
    DumpTaggedParserAtomIndexNoQuote(out, index, nullptr);
  }
  out.put("\"");
}

void LoopControlStatement::dumpImpl(const ParserAtomsTable* parserAtoms,
                                    GenericPrinter& out, int indent) {
  const char* name = parseNodeNames[getKindAsIndex()];
  out.printf("(%s", name);
  if (label_) {
    out.printf(" ");
    DumpCharsNoNewline(parserAtoms, label_, out);
  }
  out.printf(")");
}

void UnaryNode::dumpImpl(const ParserAtomsTable* parserAtoms,
                         GenericPrinter& out, int indent) {
  const char* name = parseNodeNames[getKindAsIndex()];
  out.printf("(%s ", name);
  indent += strlen(name) + 2;
  ::DumpParseTree(parserAtoms, kid(), out, indent);
  out.printf(")");
}

void BinaryNode::dumpImpl(const ParserAtomsTable* parserAtoms,
                          GenericPrinter& out, int indent) {
  if (isKind(ParseNodeKind::DotExpr)) {
    out.put("(.");

    ::DumpParseTree(parserAtoms, right(), out, indent + 2);

    out.putChar(' ');
    if (as<PropertyAccess>().isSuper()) {
      out.put("super");
    } else {
      ::DumpParseTree(parserAtoms, left(), out, indent + 2);
    }

    out.printf(")");
    return;
  }

  const char* name = parseNodeNames[getKindAsIndex()];
  out.printf("(%s ", name);
  indent += strlen(name) + 2;
  ::DumpParseTree(parserAtoms, left(), out, indent);
  IndentNewLine(out, indent);
  ::DumpParseTree(parserAtoms, right(), out, indent);
  out.printf(")");
}

void TernaryNode::dumpImpl(const ParserAtomsTable* parserAtoms,
                           GenericPrinter& out, int indent) {
  const char* name = parseNodeNames[getKindAsIndex()];
  out.printf("(%s ", name);
  indent += strlen(name) + 2;
  ::DumpParseTree(parserAtoms, kid1(), out, indent);
  IndentNewLine(out, indent);
  ::DumpParseTree(parserAtoms, kid2(), out, indent);
  IndentNewLine(out, indent);
  ::DumpParseTree(parserAtoms, kid3(), out, indent);
  out.printf(")");
}

void FunctionNode::dumpImpl(const ParserAtomsTable* parserAtoms,
                            GenericPrinter& out, int indent) {
  const char* name = parseNodeNames[getKindAsIndex()];
  out.printf("(%s ", name);
  indent += strlen(name) + 2;
  ::DumpParseTree(parserAtoms, body(), out, indent);
  out.printf(")");
}

void ModuleNode::dumpImpl(const ParserAtomsTable* parserAtoms,
                          GenericPrinter& out, int indent) {
  const char* name = parseNodeNames[getKindAsIndex()];
  out.printf("(%s ", name);
  indent += strlen(name) + 2;
  ::DumpParseTree(parserAtoms, body(), out, indent);
  out.printf(")");
}

void ListNode::dumpImpl(const ParserAtomsTable* parserAtoms,
                        GenericPrinter& out, int indent) {
  const char* name = parseNodeNames[getKindAsIndex()];
  out.printf("(%s [", name);
  if (ParseNode* listHead = head()) {
    indent += strlen(name) + 3;
    ::DumpParseTree(parserAtoms, listHead, out, indent);
    for (ParseNode* item : contentsFrom(listHead->pn_next)) {
      IndentNewLine(out, indent);
      ::DumpParseTree(parserAtoms, item, out, indent);
    }
  }
  out.printf("])");
}

void NameNode::dumpImpl(const ParserAtomsTable* parserAtoms,
                        GenericPrinter& out, int indent) {
  switch (getKind()) {
    case ParseNodeKind::StringExpr:
    case ParseNodeKind::TemplateStringExpr:
    case ParseNodeKind::ObjectPropertyName:
      DumpCharsNoNewline(parserAtoms, atom_, out);
      return;

    case ParseNodeKind::Name:
    case ParseNodeKind::PrivateName:  // atom() already includes the '#', no
                                      // need to specially include it.
    case ParseNodeKind::PropertyNameExpr:
      if (!atom_) {
        out.put("#<null name>");
      } else if (parserAtoms) {
        if (atom_ == TaggedParserAtomIndex::WellKnown::empty()) {
          out.put("#<zero-length name>");
        } else {
          parserAtoms->dumpCharsNoQuote(out, atom_);
        }
      } else {
        DumpTaggedParserAtomIndexNoQuote(out, atom_, nullptr);
      }
      return;

    case ParseNodeKind::LabelStmt: {
      this->as<LabeledStatement>().dumpImpl(parserAtoms, out, indent);
      return;
    }

    default: {
      const char* name = parseNodeNames[getKindAsIndex()];
      out.printf("(%s)", name);
      return;
    }
  }
}

void LabeledStatement::dumpImpl(const ParserAtomsTable* parserAtoms,
                                GenericPrinter& out, int indent) {
  const char* name = parseNodeNames[getKindAsIndex()];
  out.printf("(%s ", name);
  DumpCharsNoNewline(parserAtoms, label(), out);
  indent += strlen(name) + 2;
  IndentNewLine(out, indent);
  ::DumpParseTree(parserAtoms, statement(), out, indent);
  out.printf(")");
}

template <ParseNodeKind Kind, typename ScopeType>
void BaseScopeNode<Kind, ScopeType>::dumpImpl(
    const ParserAtomsTable* parserAtoms, GenericPrinter& out, int indent) {
  const char* name = parseNodeNames[getKindAsIndex()];
  out.printf("(%s [", name);
  int nameIndent = indent + strlen(name) + 3;
  if (!isEmptyScope()) {
    typename ScopeType::ParserData* bindings = scopeBindings();
    auto names = GetScopeDataTrailingNames(bindings);
    for (uint32_t i = 0; i < names.size(); i++) {
      auto index = names[i].name();
      if (parserAtoms) {
        if (index == TaggedParserAtomIndex::WellKnown::empty()) {
          out.put("#<zero-length name>");
        } else {
          parserAtoms->dumpCharsNoQuote(out, index);
        }
      } else {
        DumpTaggedParserAtomIndexNoQuote(out, index, nullptr);
      }
      if (i < names.size() - 1) {
        IndentNewLine(out, nameIndent);
      }
    }
  }
  out.putChar(']');
  indent += 2;
  IndentNewLine(out, indent);
  ::DumpParseTree(parserAtoms, scopeBody(), out, indent);
  out.printf(")");
}

#  ifdef ENABLE_DECORATORS
void ClassMethod::dumpImpl(const ParserAtomsTable* parserAtoms,
                           GenericPrinter& out, int indent) {
  if (decorators_) {
    decorators_->dumpImpl(parserAtoms, out, indent);
  }
  Base::dumpImpl(parserAtoms, out, indent);
}

void ClassField::dumpImpl(const ParserAtomsTable* parserAtoms,
                          GenericPrinter& out, int indent) {
  if (decorators_) {
    decorators_->dumpImpl(parserAtoms, out, indent);
    out.putChar(' ');
  }
  Base::dumpImpl(parserAtoms, out, indent);
  IndentNewLine(out, indent + 2);
  if (accessorGetterNode_) {
    out.printf("getter: ");
    accessorGetterNode_->dumpImpl(parserAtoms, out, indent);
  }
  IndentNewLine(out, indent + 2);
  if (accessorSetterNode_) {
    out.printf("setter: ");
    accessorSetterNode_->dumpImpl(parserAtoms, out, indent);
  }
}

void ClassNode::dumpImpl(const ParserAtomsTable* parserAtoms,
                         GenericPrinter& out, int indent) {
  if (decorators_) {
    decorators_->dumpImpl(parserAtoms, out, indent);
  }
  Base::dumpImpl(parserAtoms, out, indent);
}
#  endif

#endif

TaggedParserAtomIndex NumericLiteral::toAtom(
    FrontendContext* fc, ParserAtomsTable& parserAtoms) const {
  return NumberToParserAtom(fc, parserAtoms, value());
}

RegExpObject* RegExpLiteral::create(
    JSContext* cx, FrontendContext* fc, ParserAtomsTable& parserAtoms,
    CompilationAtomCache& atomCache,
    ExtensibleCompilationStencil& stencil) const {
  return stencil.regExpData[index_].createRegExpAndEnsureAtom(
      cx, fc, parserAtoms, atomCache);
}

bool js::frontend::IsAnonymousFunctionDefinition(ParseNode* pn) {
  // ES 2017 draft
  // 12.15.2 (ArrowFunction, AsyncArrowFunction).
  // 14.1.12 (FunctionExpression).
  // 14.4.8 (Generatoression).
  // 14.6.8 (AsyncFunctionExpression)
  if (pn->is<FunctionNode>() &&
      !pn->as<FunctionNode>().funbox()->explicitName()) {
    return true;
  }

  // 14.5.8 (ClassExpression)
  if (pn->is<ClassNode>() && !pn->as<ClassNode>().names()) {
    return true;
  }

  return false;
}
