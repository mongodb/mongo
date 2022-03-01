/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "frontend/ParseNode.h"

#include "mozilla/FloatingPoint.h"

#include "jsnum.h"

#include "frontend/CompilationStencil.h"  // ExtensibleCompilationStencil
#include "frontend/FullParseHandler.h"
#include "frontend/ParseContext.h"
#include "frontend/Parser.h"      // ParserBase
#include "frontend/ParserAtom.h"  // ParserAtomsTable, TaggedParserAtomIndex
#include "frontend/SharedContext.h"
#include "vm/BigIntType.h"
#include "vm/Printer.h"
#include "vm/RegExpObject.h"
#include "vm/Scope.h"  // GetScopeDataTrailingNames

using namespace js;
using namespace js::frontend;

using mozilla::IsFinite;

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
    ReportOutOfMemory(cx);
  }
  return p;
}

ParseNode* ParseNode::appendOrCreateList(ParseNodeKind kind, ParseNode* left,
                                         ParseNode* right,
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

  ListNode* list = handler->new_<ListNode>(kind, left);
  if (!list) {
    return nullptr;
  }

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
#  define STRINGIFY(name, _type) #  name,
    FOR_EACH_PARSE_NODE_KIND(STRINGIFY)
#  undef STRINGIFY
};

void frontend::DumpParseTree(ParserBase* parser, ParseNode* pn,
                             GenericPrinter& out, int indent) {
  if (pn == nullptr) {
    out.put("#NULL");
  } else {
    pn->dump(parser, out, indent);
  }
}

static void IndentNewLine(GenericPrinter& out, int indent) {
  out.putChar('\n');
  for (int i = 0; i < indent; ++i) {
    out.putChar(' ');
  }
}

void ParseNode::dump() { dump(nullptr); }

void ParseNode::dump(ParserBase* parser) {
  js::Fprinter out(stderr);
  dump(parser, out);
}

void ParseNode::dump(ParserBase* parser, GenericPrinter& out) {
  dump(parser, out, 0);
  out.putChar('\n');
}

void ParseNode::dump(ParserBase* parser, GenericPrinter& out, int indent) {
  switch (getKind()) {
#  define DUMP(K, T)                         \
    case ParseNodeKind::K:                   \
      as<T>().dumpImpl(parser, out, indent); \
      break;
    FOR_EACH_PARSE_NODE_KIND(DUMP)
#  undef DUMP
    default:
      out.printf("#<BAD NODE %p, kind=%u>", (void*)this, unsigned(getKind()));
  }
}

void NullaryNode::dumpImpl(ParserBase* parser, GenericPrinter& out,
                           int indent) {
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

void NumericLiteral::dumpImpl(ParserBase* parser, GenericPrinter& out,
                              int indent) {
  ToCStringBuf cbuf;
  const char* cstr = NumberToCString(nullptr, &cbuf, value());
  if (!IsFinite(value())) {
    out.put("#");
  }
  if (cstr) {
    out.printf("%s", cstr);
  } else {
    out.printf("%g", value());
  }
}

void BigIntLiteral::dumpImpl(ParserBase* parser, GenericPrinter& out,
                             int indent) {
  out.printf("(%s)", parseNodeNames[getKindAsIndex()]);
}

void RegExpLiteral::dumpImpl(ParserBase* parser, GenericPrinter& out,
                             int indent) {
  out.printf("(%s)", parseNodeNames[getKindAsIndex()]);
}

static void DumpCharsNoNewline(ParserBase* parser, TaggedParserAtomIndex index,
                               GenericPrinter& out) {
  out.put("\"");
  if (parser) {
    parser->parserAtoms().dumpCharsNoQuote(out, index);
  } else {
    DumpTaggedParserAtomIndexNoQuote(out, index, nullptr);
  }
  out.put("\"");
}

void LoopControlStatement::dumpImpl(ParserBase* parser, GenericPrinter& out,
                                    int indent) {
  const char* name = parseNodeNames[getKindAsIndex()];
  out.printf("(%s", name);
  if (label_) {
    out.printf(" ");
    DumpCharsNoNewline(parser, label_, out);
  }
  out.printf(")");
}

void UnaryNode::dumpImpl(ParserBase* parser, GenericPrinter& out, int indent) {
  const char* name = parseNodeNames[getKindAsIndex()];
  out.printf("(%s ", name);
  indent += strlen(name) + 2;
  DumpParseTree(parser, kid(), out, indent);
  out.printf(")");
}

void BinaryNode::dumpImpl(ParserBase* parser, GenericPrinter& out, int indent) {
  if (isKind(ParseNodeKind::DotExpr)) {
    out.put("(.");

    DumpParseTree(parser, right(), out, indent + 2);

    out.putChar(' ');
    if (as<PropertyAccess>().isSuper()) {
      out.put("super");
    } else {
      DumpParseTree(parser, left(), out, indent + 2);
    }

    out.printf(")");
    return;
  }

  const char* name = parseNodeNames[getKindAsIndex()];
  out.printf("(%s ", name);
  indent += strlen(name) + 2;
  DumpParseTree(parser, left(), out, indent);
  IndentNewLine(out, indent);
  DumpParseTree(parser, right(), out, indent);
  out.printf(")");
}

void TernaryNode::dumpImpl(ParserBase* parser, GenericPrinter& out,
                           int indent) {
  const char* name = parseNodeNames[getKindAsIndex()];
  out.printf("(%s ", name);
  indent += strlen(name) + 2;
  DumpParseTree(parser, kid1(), out, indent);
  IndentNewLine(out, indent);
  DumpParseTree(parser, kid2(), out, indent);
  IndentNewLine(out, indent);
  DumpParseTree(parser, kid3(), out, indent);
  out.printf(")");
}

void FunctionNode::dumpImpl(ParserBase* parser, GenericPrinter& out,
                            int indent) {
  const char* name = parseNodeNames[getKindAsIndex()];
  out.printf("(%s ", name);
  indent += strlen(name) + 2;
  DumpParseTree(parser, body(), out, indent);
  out.printf(")");
}

void ModuleNode::dumpImpl(ParserBase* parser, GenericPrinter& out, int indent) {
  const char* name = parseNodeNames[getKindAsIndex()];
  out.printf("(%s ", name);
  indent += strlen(name) + 2;
  DumpParseTree(parser, body(), out, indent);
  out.printf(")");
}

void ListNode::dumpImpl(ParserBase* parser, GenericPrinter& out, int indent) {
  const char* name = parseNodeNames[getKindAsIndex()];
  out.printf("(%s [", name);
  if (ParseNode* listHead = head()) {
    indent += strlen(name) + 3;
    DumpParseTree(parser, listHead, out, indent);
    for (ParseNode* item : contentsFrom(listHead->pn_next)) {
      IndentNewLine(out, indent);
      DumpParseTree(parser, item, out, indent);
    }
  }
  out.printf("])");
}

void NameNode::dumpImpl(ParserBase* parser, GenericPrinter& out, int indent) {
  switch (getKind()) {
    case ParseNodeKind::StringExpr:
    case ParseNodeKind::TemplateStringExpr:
    case ParseNodeKind::ObjectPropertyName:
      DumpCharsNoNewline(parser, atom_, out);
      return;

    case ParseNodeKind::Name:
    case ParseNodeKind::PrivateName:  // atom() already includes the '#', no
                                      // need to specially include it.
    case ParseNodeKind::PropertyNameExpr:
      if (!atom_) {
        out.put("#<null name>");
      } else if (parser) {
        if (atom_ == TaggedParserAtomIndex::WellKnown::empty()) {
          out.put("#<zero-length name>");
        } else {
          parser->parserAtoms().dumpCharsNoQuote(out, atom_);
        }
      } else {
        DumpTaggedParserAtomIndexNoQuote(out, atom_, nullptr);
      }
      return;

    case ParseNodeKind::LabelStmt: {
      this->as<LabeledStatement>().dumpImpl(parser, out, indent);
      return;
    }

    default: {
      const char* name = parseNodeNames[getKindAsIndex()];
      out.printf("(%s)", name);
      return;
    }
  }
}

void LabeledStatement::dumpImpl(ParserBase* parser, GenericPrinter& out,
                                int indent) {
  const char* name = parseNodeNames[getKindAsIndex()];
  out.printf("(%s ", name);
  DumpCharsNoNewline(parser, label(), out);
  indent += strlen(name) + 2;
  IndentNewLine(out, indent);
  DumpParseTree(parser, statement(), out, indent);
  out.printf(")");
}

template <ParseNodeKind Kind, typename ScopeType>
void BaseScopeNode<Kind, ScopeType>::dumpImpl(ParserBase* parser,
                                              GenericPrinter& out, int indent) {
  const char* name = parseNodeNames[getKindAsIndex()];
  out.printf("(%s [", name);
  int nameIndent = indent + strlen(name) + 3;
  if (!isEmptyScope()) {
    typename ScopeType::ParserData* bindings = scopeBindings();
    auto names = GetScopeDataTrailingNames(bindings);
    for (uint32_t i = 0; i < names.size(); i++) {
      auto index = names[i].name();
      if (parser) {
        if (index == TaggedParserAtomIndex::WellKnown::empty()) {
          out.put("#<zero-length name>");
        } else {
          parser->parserAtoms().dumpCharsNoQuote(out, index);
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
  DumpParseTree(parser, scopeBody(), out, indent);
  out.printf(")");
}
#endif

TaggedParserAtomIndex NumericLiteral::toAtom(
    JSContext* cx, ParserAtomsTable& parserAtoms) const {
  return NumberToParserAtom(cx, parserAtoms, value());
}

RegExpObject* RegExpLiteral::create(
    JSContext* cx, ParserAtomsTable& parserAtoms,
    CompilationAtomCache& atomCache,
    ExtensibleCompilationStencil& stencil) const {
  return stencil.regExpData[index_].createRegExpAndEnsureAtom(cx, parserAtoms,
                                                              atomCache);
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
