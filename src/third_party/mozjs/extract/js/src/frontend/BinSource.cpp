/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "frontend/BinSource.h"

#include "mozilla/ArrayUtils.h"
#include "mozilla/Casting.h"
#include "mozilla/Maybe.h"
#include "mozilla/Move.h"
#include "mozilla/PodOperations.h"
#include "mozilla/Vector.h"

#include "frontend/BinTokenReaderTester.h"
#include "frontend/FullParseHandler.h"
#include "frontend/Parser.h"
#include "frontend/SharedContext.h"

#include "vm/RegExpObject.h"

#include "frontend/ParseContext-inl.h"
#include "frontend/ParseNode-inl.h"


// # About compliance with EcmaScript
//
// For the moment, this parser implements ES5. Future versions will be extended
// to ES6 and further on.
//
// By design, it does NOT implement Annex B.3.3. If possible, we would like
// to avoid going down that rabbit hole.
//
//
// # About the AST
//
// At this stage of experimentation, the AST specifications change often. This
// version of the parser attempts to implement
// https://gist.github.com/Yoric/2390f0367515c079172be2526349b294
//
//
// # About validating the AST
//
// Normally, this implementation validates all properties of the AST *except* the
// order of fields, which is partially constrained by the AST spec (e.g. in a block,
// field `scope` must appear before field `body`, etc.).
//
//
// # About names and scopes
//
// One of the key objectives of the BinAST syntax is to be able to entirely skip
// parsing inner functions until they are needed. With a purely syntactic AST,
// this is generally impossible, as we would need to walk the AST to find
// lexically-bound/var-bound variables, instances of direct eval, etc.
//
// To achieve this, BinAST files contain scope data, as instances of
// `BinJS:Scope` nodes. Rather than walking the AST to assign bindings
// to scopes, we extract data from the `BinJS:Scope` and check it lazily,
// once we actually need to walk the AST.
//
// WARNING: The current implementation DOES NOT perform the check yet. It
// is therefore unsafe.
//
// # About directives
//
// Currently, directives are ignored and treated as regular strings.
//
// They should be treated lazily (whenever we open a subscope), like bindings.

// Evaluate an expression, checking that the result is not 0.
//
// Throw `cx->alreadyReportedError()` if it returns 0/nullptr.
#define TRY(EXPR) \
    do { \
        if (!EXPR) \
            return cx_->alreadyReportedError(); \
    } while(false)


#define TRY_VAR(VAR, EXPR) \
    do { \
        VAR = EXPR; \
        if (!VAR) \
            return cx_->alreadyReportedError(); \
    } while (false)

#define TRY_DECL(VAR, EXPR) \
    auto VAR = EXPR; \
    if (!VAR) \
       return cx_->alreadyReportedError();

#define TRY_EMPL(VAR, EXPR) \
    do { \
        auto _tryEmplResult = EXPR; \
        if (!_tryEmplResult) \
            return cx_->alreadyReportedError(); \
        VAR.emplace(_tryEmplResult.unwrap()); \
    } while (false)

#define MOZ_TRY_EMPLACE(VAR, EXPR) \
    do { \
        auto _tryEmplResult = EXPR; \
        if (_tryEmplResult.isErr()) \
            return ::mozilla::Err(_tryEmplResult.unwrapErr()); \
        VAR.emplace(_tryEmplResult.unwrap()); \
    } while (false)

using namespace mozilla;

namespace js {
namespace frontend {

using AutoList = BinTokenReaderTester::AutoList;
using AutoTaggedTuple = BinTokenReaderTester::AutoTaggedTuple;
using AutoTuple = BinTokenReaderTester::AutoTuple;
using BinFields = BinTokenReaderTester::BinFields;
using Chars = BinTokenReaderTester::Chars;
using NameBag = GCHashSet<JSString*>;
using Names = GCVector<JSString*, 8>;
using UsedNamePtr = UsedNameTracker::UsedNameMap::Ptr;

namespace {
    // Compare a bunch of `uint8_t` values (as returned by the tokenizer_) with
    // a string literal (and ONLY a string literal).
    template<size_t N>
    bool operator==(const Chars& left, const char (&right)[N]) {
        return BinTokenReaderTester::equals(left, right);
    }

    bool isMethod(BinKind kind) {
        switch (kind) {
          case BinKind::ObjectMethod:
          case BinKind::ObjectGetter:
          case BinKind::ObjectSetter:
            return true;
          default:
            return false;
        }
    }

#if defined(DEBUG)
    bool isMethodOrFunction(BinKind kind) {
        if (isMethod(kind))
            return true;
        if (kind == BinKind::FunctionExpression || kind == BinKind::FunctionDeclaration)
            return true;
        return false;
    }
#endif // defined(DEBUG)
}

JS::Result<ParseNode*>
BinASTParser::parse(const Vector<uint8_t>& data)
{
    return parse(data.begin(), data.length());
}

JS::Result<ParseNode*>
BinASTParser::parse(const uint8_t* start, const size_t length)
{
    auto result = parseAux(start, length);
    poison(); // Make sure that the parser is never used again accidentally.
    return result;
}


JS::Result<ParseNode*>
BinASTParser::parseAux(const uint8_t* start, const size_t length)
{
    tokenizer_.emplace(cx_, start, length);

    Directives directives(options().strictOption);
    GlobalSharedContext globalsc(cx_, ScopeKind::Global,
                                 directives, options().extraWarningsOption);
    BinParseContext globalpc(cx_, this, &globalsc, /* newDirectives = */ nullptr);
    if (!globalpc.init())
        return cx_->alreadyReportedError();

    ParseContext::VarScope varScope(cx_, &globalpc, usedNames_);
    if (!varScope.init(&globalpc))
        return cx_->alreadyReportedError();

    ParseNode* result(nullptr);
    MOZ_TRY_VAR(result, parseProgram());

    Maybe<GlobalScope::Data*> bindings = NewGlobalScopeData(cx_, varScope, alloc_, parseContext_);
    if (!bindings)
        return cx_->alreadyReportedError();
    globalsc.bindings = *bindings;

    return result; // Magic conversion to Ok.
}

Result<ParseNode*>
BinASTParser::parseProgram()
{
    BinKind kind;
    BinFields fields(cx_);
    AutoTaggedTuple guard(*tokenizer_);

    TRY(tokenizer_->enterTaggedTuple(kind, fields, guard));
    if (kind != BinKind::Program)
        return this->raiseInvalidKind("Program", kind);

    ParseNode* result;
    MOZ_TRY_VAR(result, parseBlockStatementAux(kind, fields));

    TRY(guard.done());
    return result;
}

JS::Result<ParseNode*>
BinASTParser::parseBlockStatement()
{
    BinKind kind;
    BinFields fields(cx_);
    AutoTaggedTuple guard(*tokenizer_);

    TRY(tokenizer_->enterTaggedTuple(kind, fields, guard));
    ParseNode* result(nullptr);
    switch (kind) {
      default:
        return raiseInvalidKind("BlockStatement", kind);
      case BinKind::BlockStatement:
        MOZ_TRY_VAR(result, parseBlockStatementAux(kind, fields));
        break;
    }

    TRY(guard.done());
    return result;
}

JS::Result<Ok>
BinASTParser::parseAndUpdateScopeNames(ParseContext::Scope& scope, DeclarationKind kind)
{
    AutoList guard(*tokenizer_);
    uint32_t length = 0;

    TRY(tokenizer_->enterList(length, guard));
    RootedAtom name(cx_);
    for (uint32_t i = 0; i < length; ++i) {
        name = nullptr;

        MOZ_TRY(readString(&name));
        auto ptr = scope.lookupDeclaredNameForAdd(name);
        if (ptr) {
            if (kind == DeclarationKind::Let || kind == DeclarationKind::Const)
                return raiseError("Variable redeclaration");

#if defined(DEBUG)
            // FIXME: Fix binjs-ref.
            fprintf(stderr, "Weird: `var` redeclaration. Check encoder: ");
            name->dump();
            fprintf(stderr, "\n");
#endif // defined(DEBUG)
            continue;
        }

        TRY(scope.addDeclaredName(parseContext_, ptr, name.get(), kind, tokenizer_->offset()));
    }
    TRY(guard.done());
    return Ok();
}

JS::Result<Ok>
BinASTParser::parseAndUpdateCurrentScope()
{
    return parseAndUpdateScope(parseContext_->varScope(), *parseContext_->innermostScope());
}

JS::Result<Ok>
BinASTParser::parseAndUpdateScope(ParseContext::Scope& varScope, ParseContext::Scope& letScope)
{
    BinKind kind;
    BinFields fields(cx_);
    AutoTaggedTuple guard(*tokenizer_);

    TRY(tokenizer_->enterTaggedTuple(kind, fields, guard));
    switch (kind) {
      default:
        return raiseInvalidKind("Scope", kind);
      case BinKind::BINJS_Scope:
        for (auto field : fields) {
            switch (field) {
              case BinField::BINJS_HasDirectEval:
                MOZ_TRY(readBool()); // Currently ignored.
                break;
              case BinField::BINJS_LetDeclaredNames:
                MOZ_TRY(parseAndUpdateScopeNames(letScope, DeclarationKind::Let));
                break;
              case BinField::BINJS_ConstDeclaredNames:
                MOZ_TRY(parseAndUpdateScopeNames(letScope, DeclarationKind::Const));
                break;
              case BinField::BINJS_VarDeclaredNames:
                MOZ_TRY(parseAndUpdateScopeNames(varScope, DeclarationKind::Var));
                break;
              case BinField::BINJS_CapturedNames: {
                Rooted<Maybe<Names>> names(cx_); //FIXME: Currently ignored.
                MOZ_TRY(parseStringList(&names));
                break;
              }
              default:
                return raiseInvalidField("Scope", field);
            }
        }
        break;
    }

    TRY(guard.done());
    return Ok();
}

JS::Result<ParseNode*>
BinASTParser::parseBlockStatementAux(const BinKind kind, const BinFields& fields)
{
    ParseContext::Statement stmt(parseContext_, StatementKind::Block);
    ParseContext::Scope scope(cx_, parseContext_, usedNames_);
    TRY(scope.init(parseContext_));

    ParseNode* body(nullptr);
    ParseNode* directives(nullptr); // FIXME: Largely ignored

    for (auto field : fields) {
        switch (field) {
          case BinField::BINJS_Scope:
            MOZ_TRY(parseAndUpdateCurrentScope());
            break;
          case BinField::Body:
            MOZ_TRY_VAR(body, parseStatementList());
            break;
          case BinField::Directives:
            if (kind != BinKind::Program)
                return raiseInvalidField("BlockStatement", field);
            MOZ_TRY_VAR(directives, parseDirectiveList());
            break;
          default:
            return raiseInvalidField("BlockStatement", field);
        }
    }

    // In case of absent optional fields, inject default values.
    if (!body)
        TRY_VAR(body, factory_.newStatementList(tokenizer_->pos()));

    MOZ_TRY_VAR(body, appendDirectivesToBody(body, directives));

    ParseNode* result;
    if (kind == BinKind::Program) {
        result = body;
    } else {
        TRY_DECL(bindings, NewLexicalScopeData(cx_, scope, alloc_, parseContext_));
        TRY_VAR(result, factory_.newLexicalScope(*bindings, body));
    }

    return result;
}

JS::Result<ParseNode*>
BinASTParser::appendDirectivesToBody(ParseNode* body, ParseNode* directives)
{
    ParseNode* result = body;
    if (directives && directives->pn_count >= 1) {
        MOZ_ASSERT(directives->isArity(PN_LIST));

        // Convert directive list to a list of strings.
        TRY_DECL(prefix, factory_.newStatementList(directives->pn_head->pn_pos));
        for (ParseNode* iter = directives->pn_head; iter != nullptr; iter = iter->pn_next) {
            TRY_DECL(statement, factory_.newExprStatement(iter, iter->pn_pos.end));
            prefix->appendWithoutOrderAssumption(statement);
        }

        // Prepend to the body.
        ParseNode* iter = body->pn_head;
        while (iter) {
            ParseNode* next = iter->pn_next;
            prefix->appendWithoutOrderAssumption(iter);
            iter = next;
        }
        prefix->setKind(body->getKind());
        prefix->setOp(body->getOp());
        result = prefix;
#if defined(DEBUG)
        result->checkListConsistency();
#endif // defined(DEBUG)
    }

    return result;
}

JS::Result<Ok>
BinASTParser::parseStringList(MutableHandle<Maybe<Names>> out)
{
    MOZ_ASSERT(out.get().isNothing()); // Sanity check: the node must not have been parsed yet.

    uint32_t length;
    AutoList guard(*tokenizer_);

    Names result(cx_);

    TRY(tokenizer_->enterList(length, guard));
    if (!result.reserve(length))
        return raiseOOM();

    RootedAtom string(cx_);
    for (uint32_t i = 0; i < length; ++i) {
        string = nullptr;

        MOZ_TRY(readString(&string));
        result.infallibleAppend(Move(string)); // Checked in the call to `reserve`.
    }

    TRY(guard.done());
    out.set(Move(Some(Move(result))));
    return Ok();
}

JS::Result<ParseNode*>
BinASTParser::parseStatementList()
{
    uint32_t length;
    AutoList guard(*tokenizer_);

    TRY_DECL(result, factory_.newStatementList(tokenizer_->pos()));

    TRY(tokenizer_->enterList(length, guard));
    for (uint32_t i = 0; i < length; ++i) {
        BinKind kind;
        BinFields fields(cx_);
        AutoTaggedTuple guard(*tokenizer_);

        ParseNode* statement(nullptr);

        TRY(tokenizer_->enterTaggedTuple(kind, fields, guard));
        switch (kind) {
          case BinKind::FunctionDeclaration:
            MOZ_TRY_VAR(statement, parseFunctionAux(kind, fields));
            break;
          default:
            MOZ_TRY_VAR(statement, parseStatementAux(kind, fields));
            break;
        }

        TRY(guard.done());
        result->appendWithoutOrderAssumption(statement);
    }

    TRY(guard.done());
    return result;
}

JS::Result<ParseNode*>
BinASTParser::parseStatement()
{
    BinKind kind;
    BinFields fields(cx_);
    AutoTaggedTuple guard(*tokenizer_);

    TRY(tokenizer_->enterTaggedTuple(kind, fields, guard));
    ParseNode* result;
    MOZ_TRY_VAR(result, parseStatementAux(kind, fields));

    TRY(guard.done());
    return result;
}

JS::Result<ParseNode*>
BinASTParser::parseStatementAux(const BinKind kind, const BinFields& fields)
{
    const size_t start = tokenizer_->offset();

    ParseNode* result(nullptr);
    switch (kind) {
      case BinKind::EmptyStatement: {
        TRY_VAR(result, factory_.newEmptyStatement(tokenizer_->pos(start)));
        break;
      }
      case BinKind::BlockStatement:
        MOZ_TRY_VAR(result, parseBlockStatementAux(kind, fields));
        break;
      case BinKind::ExpressionStatement:
        MOZ_TRY_VAR(result, parseExpressionStatementAux(kind, fields));
        break;
      case BinKind::DebuggerStatement: {
        TRY_VAR(result, factory_.newDebuggerStatement(tokenizer_->pos(start)));
        break;
      }
      case BinKind::WithStatement: {
        ParseNode* body(nullptr);
        ParseNode* expr(nullptr);
        for (auto field : fields) {
            switch (field) {
              case BinField::Body:
                MOZ_TRY_VAR(body, parseStatement());
                break;
              case BinField::Object:
                MOZ_TRY_VAR(expr, parseExpression());
                break;
              default:
                return raiseInvalidField("WithStatement", field);
            }
        }

        if (!body)
            return raiseMissingField("WithStatement", BinField::Body);
        if (!expr)
            return raiseMissingField("WithStatement", BinField::Object);

        TRY_VAR(result, factory_.newWithStatement(start, expr, body));

        break;
      }
      case BinKind::ReturnStatement: {
        if (!parseContext_->isFunctionBox()) {
            // Return statements are permitted only inside functions.
            return raiseInvalidKind("Toplevel Statement", kind);
        }
        parseContext_->functionBox()->usesReturn = true;

        ParseNode* arg(nullptr);
        for (auto field : fields) {
            switch (field) {
              case BinField::Argument:
                MOZ_TRY_VAR(arg, parseExpression());
                break;
              default:
                return raiseInvalidField("ReturnStatement", field);
            }
        }

        TRY_VAR(result, factory_.newReturnStatement(arg, tokenizer_->pos(start)));

        break;
      }
      case BinKind::LabeledStatement: {
        // We check for the existence of the jump target when parsing `break label;` or `continue label;`.
        ParseContext::Statement stmt(parseContext_, StatementKind::Label);
        ParseNode* label(nullptr);
        ParseNode* body(nullptr);

        for (auto field : fields) {
            switch (field) {
              case BinField::Label:
                MOZ_TRY_VAR(label, parseIdentifier());
                break;
              case BinField::Body: {
                if (!label)  // By order of fields, `label` MUST always be before `body` in the file.
                    return raiseMissingField("LabeledStatement", BinField::Label);
                MOZ_ASSERT(label->name());
                ParseContext::LabelStatement stmt(parseContext_, label->name());
                MOZ_TRY_VAR(body, parseStatement());
                break;
              }
              default:
                return raiseInvalidField("LabeledStatement", field);
            }
        }

        if (!label)
            return raiseMissingField("LabeledStatement", BinField::Label);
        if (!body)
            return raiseMissingField("LabeledStatement", BinField::Body);

        TRY_VAR(result, factory_.newLabeledStatement(label->name(), body, start));

        break;
      }

      case BinKind::BreakStatement:
      case BinKind::ContinueStatement:
        MOZ_TRY_VAR(result, parseBreakOrContinueStatementAux(kind, fields));
        break;

      case BinKind::IfStatement: {
        ParseContext::Statement stmt(parseContext_, StatementKind::If);

        ParseNode* test(nullptr);
        ParseNode* consequent(nullptr);
        ParseNode* alternate(nullptr); // Optional

        for (auto field : fields) {
            switch (field) {
              case BinField::Test:
                MOZ_TRY_VAR(test, parseExpression());
                break;
              case BinField::Consequent:
                MOZ_TRY_VAR(consequent, parseStatement());
                break;
              case BinField::Alternate:
                MOZ_TRY_VAR(alternate, parseStatement());
                break;
              default:
                return raiseInvalidField("IfStatement", field);
            }
        }

        if (!test)
            return raiseMissingField("IfStatement", BinField::Test);
        if (!consequent)
            return raiseMissingField("IfStatement", BinField::Consequent);

        TRY_VAR(result, factory_.newIfStatement(start, test, consequent, alternate));

        break;
      }
      case BinKind::SwitchStatement: {
        ParseContext::Statement stmt(parseContext_, StatementKind::Switch);
        ParseNode* discriminant(nullptr);
        ParseNode* cases(nullptr);

        for (auto field : fields) {
            switch (field) {
              case BinField::Discriminant: {
                MOZ_TRY_VAR(discriminant, parseExpression());
                break;
              }
              case BinField::Cases: {
                MOZ_TRY_VAR(cases, parseSwitchCaseList());
                break;
              }
              default:
                return raiseInvalidField("SwitchStatement", field);
            }
        }

        if (!discriminant)
            return raiseMissingField("SwitchStatement", BinField::Discriminant);
        if (!cases) {
            TRY_VAR(cases, factory_.newStatementList(tokenizer_->pos()));

            TRY_VAR(cases, factory_.newLexicalScope(nullptr, cases));
        }

        TRY_VAR(result, factory_.newSwitchStatement(start, discriminant, cases));

        break;
      }

      case BinKind::ThrowStatement: {
        ParseNode* arg(nullptr);
        for (auto field : fields) {
            if (field != BinField::Argument)
                return raiseInvalidField("ThrowStatement", field);

            MOZ_TRY_VAR(arg, parseExpression());
        }

        if (!arg)
            return raiseMissingField("ThrowStatement", BinField::Argument);

        TRY_VAR(result, factory_.newThrowStatement(arg, tokenizer_->pos(start)));

        break;
      }

      case BinKind::TryStatement: {
        ParseNode* block(nullptr);
        ParseNode* handler(nullptr);
        ParseNode* finalizer(nullptr);

        for (auto field : fields) {
            switch (field) {
              case BinField::Block: {
                ParseContext::Statement stmt(parseContext_, StatementKind::Try);
                ParseContext::Scope scope(cx_, parseContext_, usedNames_);
                TRY(scope.init(parseContext_));
                MOZ_TRY_VAR(block, parseBlockStatement());
                break;
              }
              case BinField::Handler:
                MOZ_TRY_VAR(handler, parseCatchClause());
                break;

              case BinField::Finalizer: {
                ParseContext::Statement stmt(parseContext_, StatementKind::Finally);
                ParseContext::Scope scope(cx_, parseContext_, usedNames_);
                TRY(scope.init(parseContext_));
                MOZ_TRY_VAR(finalizer, parseBlockStatement());
                break;
              }

              default:
                return raiseInvalidField("TryStatement", field);
            }
        }

        if (!block)
            return raiseMissingField("TryStatement", BinField::Handler);
        if (!handler && !finalizer)
            return raiseMissingField("TryStatement (without catch)", BinField::Finalizer);

        TRY_VAR(result, factory_.newTryStatement(start, block, handler, finalizer));
        break;
      }

      case BinKind::WhileStatement:
      case BinKind::DoWhileStatement: {
        ParseContext::Statement stmt(parseContext_, kind == BinKind::WhileStatement ? StatementKind::WhileLoop : StatementKind::DoLoop);
        ParseNode* test(nullptr);
        ParseNode* body(nullptr);

        for (auto field : fields) {
            switch (field) {
              case BinField::Test:
                MOZ_TRY_VAR(test, parseExpression());
                break;
              case BinField::Body:
                MOZ_TRY_VAR(body, parseStatement());
                break;
              default:
                return raiseInvalidField("WhileStatement | DoWhileStatement", field);
            }
        }

        if (!test)
            return raiseMissingField("WhileStatement | DoWhileStatement", BinField::Test);
        if (!body)
            return raiseMissingField("WhileStatement | DoWhileStatement", BinField::Body);

        if (kind == BinKind::WhileStatement)
            TRY_VAR(result, factory_.newWhileStatement(start, test, body));
        else
            TRY_VAR(result, factory_.newDoWhileStatement(body, test, tokenizer_->pos(start)));

        break;
      }
      case BinKind::ForStatement: {
        ParseContext::Statement stmt(parseContext_, StatementKind::ForLoop);

        // Implicit scope around the `for`, used to store `for (let x; ...; ...)`
        // or `for (const x; ...; ...)`-style declarations. Detail on the
        // declaration is stored as part of `BINJS_Scope`.
        ParseContext::Scope scope(cx_, parseContext_, usedNames_);
        TRY(scope.init(parseContext_));
        ParseNode* init(nullptr); // Optional
        ParseNode* test(nullptr); // Optional
        ParseNode* update(nullptr); // Optional
        ParseNode* body(nullptr); // Required

        for (auto field : fields) {
            switch (field) {
              case BinField::Init:
                MOZ_TRY_VAR(init, parseForInit());
                break;
              case BinField::Test:
                MOZ_TRY_VAR(test, parseExpression());
                break;
              case BinField::Update:
                MOZ_TRY_VAR(update, parseExpression());
                break;
              case BinField::BINJS_Scope: // The scope always appears before the body.
                MOZ_TRY(parseAndUpdateCurrentScope());
                break;
              case BinField::Body:
                MOZ_TRY_VAR(body, parseStatement());
                break;
              default:
                return raiseInvalidField("ForStatement", field);
            }
        }

        if (!body)
            return raiseMissingField("ForStatement", BinField::Body);

        TRY_DECL(forHead, factory_.newForHead(init, test, update, tokenizer_->pos(start)));
        TRY_VAR(result, factory_.newForStatement(start, forHead, body, /* iflags = */ 0));

        if (!scope.isEmpty()) {
            TRY_DECL(bindings, NewLexicalScopeData(cx_, scope, alloc_, parseContext_));
            TRY_VAR(result, factory_.newLexicalScope(*bindings, result));
        }

        break;
      }
      case BinKind::ForInStatement: {
        ParseContext::Statement stmt(parseContext_, StatementKind::ForInLoop);

        // Implicit scope around the `for`, used to store `for (let x in  ...)`
        // or `for (const x in ...)`-style declarations. Detail on the
        // declaration is stored as part of `BINJS_Scope`.
        ParseContext::Scope scope(cx_, parseContext_, usedNames_);
        TRY(scope.init(parseContext_));
        ParseNode* left(nullptr);
        ParseNode* right(nullptr);
        ParseNode* body(nullptr);

        for (auto field : fields) {
            switch (field) {
              case BinField::Left:
                MOZ_TRY_VAR(left, parseForInInit());
                break;
              case BinField::Right:
                MOZ_TRY_VAR(right, parseExpression());
                break;
              case BinField::Body:
                MOZ_TRY_VAR(body, parseStatement());
                break;
              case BinField::BINJS_Scope:
                MOZ_TRY(parseAndUpdateCurrentScope());
                break;
              default:
                return raiseInvalidField("ForInStatement", field);
            }
        }

        if (!left)
            return raiseMissingField("ForInStatement", BinField::Left);
        if (!right)
            return raiseMissingField("ForInStatement", BinField::Right);
        if (!body)
            return raiseMissingField("ForInStatement", BinField::Body);

        TRY_DECL(forHead, factory_.newForInOrOfHead(ParseNodeKind::ForIn, left, right,
                                                    tokenizer_->pos(start)));
        TRY_VAR(result, factory_.newForStatement(start, forHead, body, /*flags*/ 0));

        if (!scope.isEmpty()) {
            TRY_DECL(bindings, NewLexicalScopeData(cx_, scope, alloc_, parseContext_));
            TRY_VAR(result, factory_.newLexicalScope(*bindings, result));
        }
        break;
      }

      case BinKind::VariableDeclaration:
        MOZ_TRY_VAR(result, parseVariableDeclarationAux(kind, fields));
        break;

      default:
        return raiseInvalidKind("Statement", kind);
    }

    return result;
}

JS::Result<ParseNode*>
BinASTParser::parseBreakOrContinueStatementAux(const BinKind kind, const BinFields& fields)
{
    const auto start = tokenizer_->offset();
    ParseNode* label(nullptr);

    for (auto field : fields) {
        switch (field) {
          case BinField::Label:
            MOZ_TRY_VAR(label, parsePattern());

            if (label && !label->isKind(ParseNodeKind::Name))
                return raiseError("ContinueStatement | BreakStatement - Label MUST be an identifier"); // FIXME: This should be changed in the grammar.

            break;
          default:
            return raiseInvalidField("ContinueStatement", field);
        }
    }

    TokenPos pos = tokenizer_->pos(start);
    ParseNode* result;
    if (kind == BinKind::ContinueStatement) {
#if 0 // FIXME: We probably need to fix the AST before making this check.
        auto validity = parseContext_->checkContinueStatement(label ? label->name() : nullptr);
        if (validity.isErr()) {
            switch (validity.unwrapErr()) {
              case ParseContext::ContinueStatementError::NotInALoop:
                return raiseError(kind, "Not in a loop");
              case ParseContext::ContinueStatementError::LabelNotFound:
                return raiseError(kind, "Label not found");
            }
        }
#endif // 0
        // Ok, this is a valid continue statement.
        TRY_VAR(result, factory_.newContinueStatement(label ? label->name() : nullptr, pos));
    } else {
#if 0 // FIXME: We probably need to fix the AST before making this check.
        auto validity = parseContext_->checkBreakStatement(label ? label->name() : nullptr);
        if (validity.isErr()) {
            switch (validity.unwrapErr()) {
              case ParseContext::BreakStatementError::ToughBreak:
                 return raiseError(kind, "Not in a loop");
              case ParseContext::BreakStatementError::LabelNotFound:
                 return raiseError(kind, "Label not found");
            }
        }
#endif // 0
        // Ok, this is a valid break statement.
        TRY_VAR(result, factory_.newBreakStatement(label ? label->name() : nullptr, pos));
    }

    MOZ_ASSERT(result);

    return result;
}

JS::Result<ParseNode*>
BinASTParser::parseForInit()
{
    // This can be either a VarDecl or an Expression.
    BinFields fields(cx_);
    AutoTaggedTuple guard(*tokenizer_);
    BinKind kind;

    TRY(tokenizer_->enterTaggedTuple(kind, fields, guard));
    ParseNode* result(nullptr);

    switch (kind) {
      case BinKind::VariableDeclaration:
        MOZ_TRY_VAR(result, parseVariableDeclarationAux(kind, fields));
        break;
      default:
        // Parse as expression
        MOZ_TRY_VAR(result, parseExpressionAux(kind, fields));
        break;
    }

    TRY(guard.done());
    return result;
}

JS::Result<ParseNode*>
BinASTParser::parseForInInit()
{
    // This can be either a VarDecl or a Pattern.
    BinFields fields(cx_);
    AutoTaggedTuple guard(*tokenizer_);
    BinKind kind;

    TRY(tokenizer_->enterTaggedTuple(kind, fields, guard));
    ParseNode* result(nullptr);

    switch (kind) {
      case BinKind::VariableDeclaration:
        MOZ_TRY_VAR(result, parseVariableDeclarationAux(kind, fields));
        break;
      default:
        // Parse as expression. Not a joke: http://www.ecma-international.org/ecma-262/5.1/index.html#sec-12.6.4 .
        MOZ_TRY_VAR(result, parseExpressionAux(kind, fields));
        break;
    }

    TRY(guard.done());
    return result;
}

JS::Result<ParseNode*>
BinASTParser::parseFunctionAux(const BinKind kind, const BinFields& fields)
{
    MOZ_ASSERT(isMethodOrFunction(kind));

    const size_t start = tokenizer_->offset();

    ParseNode* id(nullptr);
    ParseNode* params(nullptr);
    ParseNode* body(nullptr);
    ParseNode* directives(nullptr); // Largely ignored for the moment.
    ParseNode* key(nullptr);  // Methods only

    // Allocate the function before walking down the tree.
    RootedFunction fun(cx_);
    TRY_VAR(fun, NewFunctionWithProto(cx_,
            /*native*/nullptr,
            /*nargs ?*/0,
            /*flags */ JSFunction::INTERPRETED_NORMAL,
            /*enclosing env*/nullptr,
            /*name*/ nullptr, // Will be known later
            /*proto*/ nullptr,
            /*alloc*/gc::AllocKind::FUNCTION,
            TenuredObject
    ));
    TRY_DECL(funbox, alloc_.new_<FunctionBox>(cx_,
        traceListHead_,
        fun,
        /* toStringStart = */0,
        /* directives = */Directives(parseContext_),
        /* extraWarning = */false,
        GeneratorKind::NotGenerator,
        FunctionAsyncKind::SyncFunction
    ));

    traceListHead_ = funbox;

    FunctionSyntaxKind syntax;
    switch (kind) {
      case BinKind::FunctionDeclaration:
        syntax = Statement;
        break;
      case BinKind::FunctionExpression:
        syntax = PrimaryExpression; // FIXME: Probably doesn't work.
        break;
      case BinKind::ObjectMethod:
        syntax = Method;
        break;
      case BinKind::ObjectGetter:
        syntax = Getter;
        break;
      case BinKind::ObjectSetter:
        syntax = Setter;
        break;
      default:
        MOZ_CRASH("Invalid FunctionSyntaxKind"); // Checked above.
    }
    funbox->initWithEnclosingParseContext(parseContext_, syntax);

    // Container scopes.
    ParseContext::Scope& varScope = parseContext_->varScope();
    ParseContext::Scope* letScope = parseContext_->innermostScope();

    // Push a new ParseContext.
    BinParseContext funpc(cx_, this, funbox, /* newDirectives = */ nullptr);
    TRY(funpc.init());
    parseContext_->functionScope().useAsVarScope(parseContext_);
    MOZ_ASSERT(parseContext_->isFunctionBox());

    for (auto field : fields) {
        switch (field) {
          case BinField::Id:
            MOZ_TRY_VAR(id, parseIdentifier());
            break;
          case BinField::Params:
            MOZ_TRY_VAR(params, parseArgumentList());
            break;
          case BinField::BINJS_Scope:
            // This scope information affects the scopes contained in the function body. MUST appear before the `body`.
            MOZ_TRY(parseAndUpdateScope(varScope, *letScope));
            break;
          case BinField::Directives:
            MOZ_TRY_VAR(directives, parseDirectiveList());
            break;
          case BinField::Body:
            MOZ_TRY_VAR(body, parseBlockStatement());
            break;
          case BinField::Key:
            if (!isMethod(kind))
                return raiseInvalidField("Functions (unless defined as methods)", field);

            MOZ_TRY_VAR(key, parseObjectPropertyName());
            break;
          default:
            return raiseInvalidField("Function", field);
        }
    }

    // Inject default values for absent fields.
    if (!params)
        TRY_VAR(params, new_<ListNode>(ParseNodeKind::ParamsBody, tokenizer_->pos()));

    if (!body)
        TRY_VAR(body, factory_.newStatementList(tokenizer_->pos()));

    if (kind == BinKind::FunctionDeclaration && !id) {
        // The name is compulsory only for function declarations.
        return raiseMissingField("FunctionDeclaration", BinField::Id);
    }

    // Reject if required values are missing.
    if (isMethod(kind) && !key)
        return raiseMissingField("method", BinField::Key);

    if (id)
        fun->initAtom(id->pn_atom);

    MOZ_ASSERT(params->isArity(PN_LIST));

    if (!(body->isKind(ParseNodeKind::LexicalScope) &&
          body->pn_u.scope.body->isKind(ParseNodeKind::StatementList)))
    {
        // Promote to lexical scope + statement list.
        if (!body->isKind(ParseNodeKind::StatementList)) {
            TRY_DECL(list, factory_.newStatementList(tokenizer_->pos(start)));

            list->initList(body);
            body = list;
        }

        // Promote to lexical scope.
        TRY_VAR(body, factory_.newLexicalScope(nullptr, body));
    }
    MOZ_ASSERT(body->isKind(ParseNodeKind::LexicalScope));

    MOZ_TRY_VAR(body, appendDirectivesToBody(body, directives));

    params->appendWithoutOrderAssumption(body);

    TokenPos pos = tokenizer_->pos(start);
    TRY_DECL(function, kind == BinKind::FunctionDeclaration
                       ? factory_.newFunctionStatement(pos)
                       : factory_.newFunctionExpression(pos));

    factory_.setFunctionBox(function, funbox);
    factory_.setFunctionFormalParametersAndBody(function, params);

    ParseNode* result;
    if (kind == BinKind::ObjectMethod)
        TRY_VAR(result, factory_.newObjectMethodOrPropertyDefinition(key, function, AccessorType::None));
    else if (kind == BinKind::ObjectGetter)
        TRY_VAR(result, factory_.newObjectMethodOrPropertyDefinition(key, function, AccessorType::Getter));
    else if (kind == BinKind::ObjectSetter)
        TRY_VAR(result, factory_.newObjectMethodOrPropertyDefinition(key, function, AccessorType::Setter));
    else
        result = function;

    // Now handle bindings.
    HandlePropertyName dotThis = cx_->names().dotThis;
    const bool declareThis = hasUsedName(dotThis) || funbox->bindingsAccessedDynamically() || funbox->isDerivedClassConstructor();

    if (declareThis) {
        ParseContext::Scope& funScope = parseContext_->functionScope();
        ParseContext::Scope::AddDeclaredNamePtr p = funScope.lookupDeclaredNameForAdd(dotThis);
        MOZ_ASSERT(!p);
        TRY(funScope.addDeclaredName(parseContext_, p, dotThis, DeclarationKind::Var,
                                      DeclaredNameInfo::npos));
        funbox->setHasThisBinding();
    }

    TRY_DECL(bindings,
             NewFunctionScopeData(cx_, parseContext_->functionScope(),
                                  /* hasParameterExprs = */false, alloc_, parseContext_));

    funbox->functionScopeBindings().set(*bindings);
    return result;
}

JS::Result<ParseNode*>
BinASTParser::parseObjectPropertyName()
{
    auto start = tokenizer_->offset();

    BinFields fields(cx_);
    AutoTaggedTuple guard(*tokenizer_);
    BinKind kind;

    TRY(tokenizer_->enterTaggedTuple(kind, fields, guard));
    ParseNode* result;
    switch (kind) {
      case BinKind::StringLiteral: {
        ParseNode* string;
        MOZ_TRY_VAR(string, parseStringLiteralAux(kind, fields));
        uint32_t index;
        if (string->pn_atom->isIndex(&index))
            TRY_VAR(result, factory_.newNumber(index, NoDecimal, TokenPos(start, tokenizer_->offset())));
        else
            result = string;

        break;
      }
      case BinKind::NumericLiteral:
        MOZ_TRY_VAR(result, parseNumericLiteralAux(kind, fields));
        break;
      case BinKind::Identifier:
        MOZ_TRY_VAR(result, parseIdentifierAux(kind, fields, /* expectObjectPropertyName = */ true));
        break;
      case BinKind::ComputedPropertyName: {
        ParseNode* expr;
        MOZ_TRY_VAR(expr, parseExpressionAux(kind, fields));
        TRY_VAR(result, factory_.newComputedName(expr, start, tokenizer_->offset()));
        break;
      }
      default:
        return raiseInvalidKind("ObjectLiteralPropertyName", kind);
    }

    TRY(guard.done());
    return result;
}

JS::Result<ParseNode*>
BinASTParser::parseVariableDeclarationAux(const BinKind kind, const BinFields& fields)
{
    const size_t start = tokenizer_->offset();

    ParseNode* result(nullptr);
    switch (kind) {
      default:
        return raiseInvalidKind("VariableDeclaration", kind);
      case BinKind::VariableDeclaration:
        ParseNodeKind pnk = ParseNodeKind::Limit;

        for (auto field : fields) {
            switch (field) {
              case BinField::Kind: {
                Maybe<Chars> kindName;
                MOZ_TRY(readString(kindName));

                if (*kindName == "let")
                    pnk = ParseNodeKind::Let;
                else if (*kindName == "var")
                    pnk = ParseNodeKind::Var;
                else if (*kindName == "const")
                    pnk = ParseNodeKind::Const;
                else
                    return raiseInvalidEnum("VariableDeclaration", *kindName);

                break;
              }
              case BinField::Declarations: {
                uint32_t length;
                AutoList guard(*tokenizer_);

                TRY(tokenizer_->enterList(length, guard));
                TRY_VAR(result, factory_.newDeclarationList(ParseNodeKind::Const /*Placeholder*/,
                                                            tokenizer_->pos(start)));

                for (uint32_t i = 0; i < length; ++i) {
                    ParseNode* current;
                    MOZ_TRY_VAR(current, parseVariableDeclarator());
                    MOZ_ASSERT(current);

                    result->appendWithoutOrderAssumption(current);
                }

                TRY(guard.done());
                break;
              }
              default:
                return raiseInvalidField("VariableDeclaration", field);
            }
        }

        if (!result || pnk == ParseNodeKind::Limit)
            return raiseMissingField("VariableDeclaration", BinField::Declarations);

        result->setKind(pnk);
    }

    return result;
}


JS::Result<ParseNode*>
BinASTParser::parseExpressionStatementAux(const BinKind kind, const BinFields& fields)
{
    MOZ_ASSERT(kind == BinKind::ExpressionStatement);

    ParseNode* expr(nullptr);
    for (auto field : fields) {
        switch (field) {
          case BinField::Expression:
            MOZ_TRY_VAR(expr, parseExpression());

            break;
          default:
            return raiseInvalidField("ExpressionStatement", field);
        }
    }

    if (!expr)
        return raiseMissingField("ExpressionStatement", BinField::Expression);

    TRY_DECL(result, factory_.newExprStatement(expr, tokenizer_->offset()));
    return result;
}

JS::Result<ParseNode*>
BinASTParser::parseVariableDeclarator()
{
    const size_t start = tokenizer_->offset();

    BinKind kind;
    BinFields fields(cx_);
    AutoTaggedTuple guard(*tokenizer_);

    TRY(tokenizer_->enterTaggedTuple(kind, fields, guard));
    if (kind != BinKind::VariableDeclarator)
        return raiseInvalidKind("VariableDeclarator", kind);

    ParseNode* id(nullptr);
    ParseNode* init(nullptr); // Optional.
    for (auto field : fields) {
        switch (field) {
          case BinField::Id:
            MOZ_TRY_VAR(id, parsePattern());

            break;
          case BinField::Init:
            MOZ_TRY_VAR(init, parseExpression());

            break;
          default:
            return raiseInvalidField("VariableDeclarator", field);
        }
    }

    TRY(guard.done());
    if (!id)
        return raiseMissingField("VariableDeclarator", BinField::Id);

    ParseNode* result(nullptr);

    // FIXME: Documentation in ParseNode is clearly obsolete.
    if (id->isKind(ParseNodeKind::Name)) {
        // `var foo [= bar]``
        TRY_VAR(result, factory_.newName(id->pn_atom->asPropertyName(), tokenizer_->pos(start), cx_));

        if (init)
            result->pn_expr = init;

    } else {
        // `var pattern = bar`
        if (!init) {
            // Here, `init` is required.
            return raiseMissingField("VariableDeclarator (with non-trivial pattern)", BinField::Init);
        }

        TRY_VAR(result, factory_.newAssignment(ParseNodeKind::Assign, id, init));
    }

    return result;
}

JS::Result<ParseNode*>
BinASTParser::parseExpressionList(bool acceptElisions)
{
    const size_t start = tokenizer_->offset();

    uint32_t length;
    AutoList guard(*tokenizer_);

    TRY(tokenizer_->enterList(length, guard));
    TRY_DECL(result, factory_.newArrayLiteral(start));

    for (uint32_t i = 0; i < length; ++i) {
        BinFields fields(cx_);
        AutoTaggedTuple guard(*tokenizer_);
        BinKind kind;

        TRY(tokenizer_->enterTaggedTuple(kind, fields, guard));
        switch (kind) {
          case BinKind::Elision: {
            if (!acceptElisions)
                return raiseInvalidKind("[Expression]", kind);

            MOZ_TRY(parseElisionAux(kind, fields));
            TRY(!factory_.addElision(result, tokenizer_->pos(start)));
            break;
          }
          default: {
            ParseNode* expr(nullptr);
            MOZ_TRY_VAR(expr, parseExpressionAux(kind, fields));

            MOZ_ASSERT(expr);
            factory_.addArrayElement(result, expr);
          }
        }

        TRY(guard.done());
    }

    TRY(guard.done());
    return result;
}

JS::Result<Ok>
BinASTParser::parseElisionAux(const BinKind kind, const BinFields& fields)
{
    MOZ_ASSERT(kind == BinKind::Elision);
    MOZ_TRY(checkEmptyTuple(kind, fields));

    return Ok();
}

JS::Result<ParseNode*>
BinASTParser::parseSwitchCaseList()
{
    uint32_t length;
    AutoList guard(*tokenizer_);

    TRY(tokenizer_->enterList(length, guard));
    TRY_DECL(list, factory_.newStatementList(tokenizer_->pos()));

    // Set to `true` once we have encountered a `default:` case.
    // Two `default:` cases is an error.
    bool haveDefault = false;

    for (uint32_t i = 0; i < length; ++i) {
        ParseNode* caseNode(nullptr);
        MOZ_TRY_VAR(caseNode, parseSwitchCase());
        MOZ_ASSERT(caseNode);

        if (caseNode->pn_left == nullptr) {
            // Ah, seems that we have encountered a default case.
            if (haveDefault) {
                // Oh, wait, two defaults? That's an error.
                return raiseError("This switch() has more than one `default:` case");
            }
            haveDefault = true;
        }
        factory_.addCaseStatementToList(list, caseNode);
    }

    TRY(guard.done());
    TRY_DECL(result, factory_.newLexicalScope(nullptr, list));

    return result;
}

JS::Result<ParseNode*>
BinASTParser::parseExpression()
{
    BinFields fields(cx_);
    AutoTaggedTuple guard(*tokenizer_);
    BinKind kind;

    TRY(tokenizer_->enterTaggedTuple(kind, fields, guard));
    ParseNode* result(nullptr);
    MOZ_TRY_VAR(result, parseExpressionAux(kind, fields));

    TRY(guard.done());
    return result;
}

JS::Result<ParseNode*>
BinASTParser::parseExpressionAux(const BinKind kind, const BinFields& fields)
{
    const size_t start = tokenizer_->offset();

    ParseNode* result(nullptr);

    switch (kind) {
      case BinKind::Identifier: {
        MOZ_TRY_VAR(result, parseIdentifierAux(kind, fields));
        break;
      }
      case BinKind::BooleanLiteral: {
        Maybe<bool> value;
        for (auto field : fields) {
            switch (field) {
              case BinField::Value:
                MOZ_TRY_EMPLACE(value, readBool());
                break;
              default:
                return raiseInvalidField("BooleanLiteral", field);
            }
        }

        // In case of absent optional fields, inject default values.
        if (!value)
            value.emplace(false);

        TRY_VAR(result, factory_.newBooleanLiteral(*value, tokenizer_->pos(start)));

        break;
      }
      case BinKind::NullLiteral: {
        MOZ_TRY(checkEmptyTuple(kind, fields));
        TRY_VAR(result, factory_.newNullLiteral(tokenizer_->pos(start)));
        break;
      }
      case BinKind::NumericLiteral:
        MOZ_TRY_VAR(result, parseNumericLiteralAux(kind, fields));
        break;

      case BinKind::RegExpLiteral: {
        RootedAtom pattern(cx_);
        Maybe<Chars> flags;
        for (auto field : fields) {
            switch (field) {
              case BinField::Pattern:
                MOZ_TRY(readString(&pattern));
                break;
              case BinField::Flags:
                MOZ_TRY(readString(flags));
                break;
              default:
                return raiseInvalidField("RegExpLiteral", field);
            }
        }

        if (!pattern)
            return raiseMissingField("RegExpLiteral", BinField::Pattern);
        if (!flags)
            return raiseMissingField("RegExpLiteral", BinField::Flags);

        RegExpFlag reflags = NoFlags;
        for (auto c : *flags) {
            if (c == 'g' && !(reflags & GlobalFlag))
                reflags = RegExpFlag(reflags | GlobalFlag);
            else if (c == 'i' && !(reflags & IgnoreCaseFlag))
                reflags = RegExpFlag(reflags | IgnoreCaseFlag);
            else if (c == 'm' && !(reflags & MultilineFlag))
                reflags = RegExpFlag(reflags | MultilineFlag);
            else if (c == 'y' && !(reflags & StickyFlag))
                reflags = RegExpFlag(reflags | StickyFlag);
            else if (c == 'u' && !(reflags & UnicodeFlag))
                reflags = RegExpFlag(reflags | UnicodeFlag);
            else
                return raiseInvalidEnum("RegExpLiteral", *flags);
        }


        Rooted<RegExpObject*> reobj(cx_);
        TRY_VAR(reobj, RegExpObject::create(cx_,
            pattern,
            reflags,
            alloc_,
            TenuredObject));

        TRY_VAR(result, factory_.newRegExp(reobj, tokenizer_->pos(start), *this));

        break;
      }
      case BinKind::StringLiteral:
        MOZ_TRY_VAR(result, parseStringLiteralAux(kind, fields));
        break;

      case BinKind::ThisExpression: {
        MOZ_TRY(checkEmptyTuple(kind, fields));

        if (parseContext_->isFunctionBox())
            parseContext_->functionBox()->usesThis = true;

        TokenPos pos = tokenizer_->pos(start);
        ParseNode* thisName(nullptr);
        if (parseContext_->sc()->thisBinding() == ThisBinding::Function)
            TRY_VAR(thisName, factory_.newName(cx_->names().dotThis, pos, cx_));

        TRY_VAR(result, factory_.newThisLiteral(pos, thisName));
        break;
      }
      case BinKind::ArrayExpression:
        MOZ_TRY_VAR(result, parseArrayExpressionAux(kind, fields));
        break;

      case BinKind::ObjectExpression:
        MOZ_TRY_VAR(result, parseObjectExpressionAux(kind, fields));
        break;

      case BinKind::FunctionExpression:
        MOZ_TRY_VAR(result, parseFunctionAux(kind, fields));
        result->setOp(JSOP_LAMBDA);
        break;

      case BinKind::UnaryExpression:
      case BinKind::UpdateExpression: {
        ParseNode* expr(nullptr);
        Maybe<Chars> operation;
        Maybe<bool> prefix; // FIXME: Ignored for unary_expression?

        for (auto field : fields) {
            switch (field) {
              case BinField::Operator:
                MOZ_TRY(readString(operation));
                break;
              case BinField::Prefix:
                MOZ_TRY_EMPLACE(prefix, readBool());
                break;
              case BinField::Argument:
                  // arguments are always parsed *after* operator.
                  if (operation.isNothing())
                      return raiseMissingField("UpdateExpression", BinField::Operator);
                MOZ_TRY_VAR(expr, parseExpression());
                break;
              default:
                return raiseInvalidField("UpdateExpression", field);
            }
        }

        if (!expr)
            return raiseMissingField("UpdateExpression", BinField::Argument);
        if (operation.isNothing())
            return raiseMissingField("UpdateExpression", BinField::Operator);

        // In case of absent optional fields, inject default values.
        if (prefix.isNothing())
            prefix.emplace(false);

        ParseNodeKind pnk = ParseNodeKind::Limit;
        if (kind == BinKind::UnaryExpression) {
            if (*operation == "-") {
                pnk = ParseNodeKind::Neg;
            } else if (*operation == "+") {
                pnk = ParseNodeKind::Pos;
            } else if (*operation == "!") {
                pnk = ParseNodeKind::Not;
            } else if (*operation == "~") {
                pnk = ParseNodeKind::BitNot;
            } else if (*operation == "typeof") {
                if (expr->isKind(ParseNodeKind::Name))
                    pnk = ParseNodeKind::TypeOfName;
                else
                    pnk = ParseNodeKind::TypeOfExpr;
            } else if (*operation == "void") {
                pnk = ParseNodeKind::Void;
            } else if (*operation == "delete") {
                switch (expr->getKind()) {
                  case ParseNodeKind::Name:
                    expr->setOp(JSOP_DELNAME);
                    pnk = ParseNodeKind::DeleteName;
                    break;
                  case ParseNodeKind::Dot:
                    pnk = ParseNodeKind::DeleteProp;
                    break;
                  case ParseNodeKind::Elem:
                    pnk = ParseNodeKind::DeleteElem;
                    break;
                  default:
                    pnk = ParseNodeKind::DeleteExpr;
                }
            } else {
                return raiseInvalidEnum("UnaryOperator", *operation);
            }
        } else if (kind == BinKind::UpdateExpression) {
            if (!expr->isKind(ParseNodeKind::Name) && !factory_.isPropertyAccess(expr))
                return raiseError("Invalid increment/decrement operand"); // FIXME: Shouldn't this be part of the syntax?

            if (*operation == "++") {
                if (*prefix)
                    pnk = ParseNodeKind::PreIncrement;
                else
                    pnk = ParseNodeKind::PostIncrement;
            } else if (*operation == "--") {
                if (*prefix)
                    pnk = ParseNodeKind::PreDecrement;
                else
                    pnk = ParseNodeKind::PostDecrement;
            } else {
                return raiseInvalidEnum("UpdateOperator", *operation);
            }
        }

        TRY_VAR(result, factory_.newUnary(pnk, start, expr));

        break;
      }
      case BinKind::BinaryExpression:
      case BinKind::LogicalExpression: {
        ParseNode* left(nullptr);
        ParseNode* right(nullptr);
        Maybe<Chars> operation;
        for (auto field : fields) {
            switch (field) {
              case BinField::Left:
                MOZ_TRY_VAR(left, parseExpression());
                break;
              case BinField::Right:
                MOZ_TRY_VAR(right, parseExpression());
                break;
              case BinField::Operator:
                MOZ_TRY(readString(operation));
                break;
              default:
                return raiseInvalidField("LogicalExpression | BinaryExpression", field);
            }
        }

        if (!left)
            return raiseMissingField("LogicalExpression | BinaryExpression", BinField::Left);
        if (!right)
            return raiseMissingField("LogicalExpression | BinaryExpression", BinField::Right);
        if (operation.isNothing())
            return raiseMissingField("LogicalExpression | BinaryExpression", BinField::Operator);

        // FIXME: Instead of Chars, we should use atoms and comparison
        // between atom ptr.
        ParseNodeKind pnk = ParseNodeKind::Limit;
        if (*operation == "==")
            pnk = ParseNodeKind::Eq;
        else if (*operation == "!=")
            pnk = ParseNodeKind::Ne;
        else if (*operation == "===")
            pnk = ParseNodeKind::StrictEq;
        else if (*operation == "!==")
            pnk = ParseNodeKind::StrictNe;
        else if (*operation == "<")
            pnk = ParseNodeKind::Lt;
        else if (*operation == "<=")
            pnk = ParseNodeKind::Le;
        else if (*operation == ">")
            pnk = ParseNodeKind::Gt;
        else if (*operation == ">=")
            pnk = ParseNodeKind::Ge;
        else if (*operation == "<<")
            pnk = ParseNodeKind::Lsh;
        else if (*operation == ">>")
            pnk = ParseNodeKind::Rsh;
        else if (*operation == ">>>")
            pnk = ParseNodeKind::Ursh;
        else if (*operation == "+")
            pnk = ParseNodeKind::Add;
        else if (*operation == "-")
            pnk = ParseNodeKind::Sub;
        else if (*operation == "*")
            pnk = ParseNodeKind::Star;
        else if (*operation == "/")
            pnk = ParseNodeKind::Div;
        else if (*operation == "%")
            pnk = ParseNodeKind::Mod;
        else if (*operation == "|")
            pnk = ParseNodeKind::BitOr;
        else if (*operation == "^")
            pnk = ParseNodeKind::BitXor;
        else if (*operation == "&")
            pnk = ParseNodeKind::BitAnd;
        else if (*operation == "in")
            pnk = ParseNodeKind::In;
        else if (*operation == "instanceof")
            pnk = ParseNodeKind::InstanceOf;
        else if (*operation == "||")
            pnk = ParseNodeKind::Or;
        else if (*operation == "&&")
            pnk = ParseNodeKind::And;
        else if (*operation == "**")
            pnk = ParseNodeKind::Pow;
        else
            return raiseInvalidEnum("BinaryOperator | LogicalOperator", *operation);

        if (left->isKind(pnk) &&
            pnk != ParseNodeKind::Pow /* ParseNodeKind::Pow is not left-associative */)
        {
            // Regroup left-associative operations into lists.
            left->appendWithoutOrderAssumption(right);
            result = left;
        } else {
            TRY_DECL(list, factory_.newList(pnk, tokenizer_->pos(start)));

            list->appendWithoutOrderAssumption(left);
            list->appendWithoutOrderAssumption(right);
            result = list;
        }

         break;
      }
      case BinKind::AssignmentExpression: {
        ParseNode* left(nullptr);
        ParseNode* right(nullptr);
        Maybe<Chars> operation;
        for (auto field : fields) {
            switch (field) {
              case BinField::Left:
                MOZ_TRY_VAR(left, parseExpression());
                break;
              case BinField::Right:
                MOZ_TRY_VAR(right, parseExpression());
                break;
              case BinField::Operator:
                MOZ_TRY(readString(operation));
                break;
              default:
                return raiseInvalidField("AssignmentExpression", field);
            }
        }

        if (!left)
            return raiseMissingField("AssignmentExpression", BinField::Left);
        if (!right)
            return raiseMissingField("AssignmentExpression", BinField::Right);
        if (operation.isNothing())
            return raiseMissingField("AssignmentExpression", BinField::Operator);

        // FIXME: Instead of Chars, we should use atoms and comparison
        // between atom ptr.
        // FIXME: We should probably turn associative operations into lists.
        ParseNodeKind pnk = ParseNodeKind::Limit;
        if (*operation == "=")
            pnk = ParseNodeKind::Assign;
        else if (*operation == "+=")
            pnk = ParseNodeKind::AddAssign;
        else if (*operation == "-=")
            pnk = ParseNodeKind::SubAssign;
        else if (*operation == "*=")
            pnk = ParseNodeKind::MulAssign;
        else if (*operation == "/=")
            pnk = ParseNodeKind::DivAssign;
        else if (*operation == "%=")
            pnk = ParseNodeKind::ModAssign;
        else if (*operation == "<<=")
            pnk = ParseNodeKind::LshAssign;
        else if (*operation == ">>=")
            pnk = ParseNodeKind::RshAssign;
        else if (*operation == ">>>=")
            pnk = ParseNodeKind::UrshAssign;
        else if (*operation == "|=")
            pnk = ParseNodeKind::BitOrAssign;
        else if (*operation == "^=")
            pnk = ParseNodeKind::BitXorAssign;
        else if (*operation == "&=")
            pnk = ParseNodeKind::BitAndAssign;
        else
            return raiseInvalidEnum("AssignmentOperator", *operation);

        TRY_VAR(result, factory_.newAssignment(pnk, left, right));

        break;
      }
      case BinKind::BracketExpression:
      case BinKind::DotExpression:
        MOZ_TRY_VAR(result, parseMemberExpressionAux(kind, fields));

        break;
      case BinKind::ConditionalExpression: {
        ParseNode* test(nullptr);
        ParseNode* alternate(nullptr);
        ParseNode* consequent(nullptr);

        for (auto field : fields) {
            switch (field) {
              case BinField::Test:
                MOZ_TRY_VAR(test, parseExpression());
                break;
              case BinField::Consequent:
                MOZ_TRY_VAR(consequent, parseExpression());
                break;
              case BinField::Alternate:
                MOZ_TRY_VAR(alternate, parseExpression());
                break;
              default:
                return raiseInvalidField("ConditionalExpression", field);
            }
        }

        if (!test)
            return raiseMissingField("ConditionalExpression", BinField::Test);
        if (!consequent)
            return raiseMissingField("ConditionalExpression", BinField::Consequent);
        if (!alternate)
            return raiseMissingField("ConditionalExpression", BinField::Alternate);

        TRY_VAR(result, factory_.newConditional(test, consequent, alternate));

        break;
      }
      case BinKind::CallExpression:
      case BinKind::NewExpression: {
        ParseNode* callee(nullptr);

        for (auto field : fields) {
            switch (field) {
              case BinField::Callee:
                MOZ_TRY_VAR(callee, parseExpression());
                break;
              case BinField::Arguments:
                MOZ_TRY_VAR(result, parseExpressionList(/* acceptElisions = */ false));
                break;
              default:
                return raiseInvalidField("NewExpression", field);
            }
        }

        // In case of absent required fields, fail.
        if (!callee)
            return raiseMissingField("NewExpression", BinField::Callee);

        // In case of absent optional fields, inject default values.
        if (!result)
            TRY_VAR(result, factory_.newArrayLiteral(start));

        ParseNodeKind pnk =
            kind == BinKind::CallExpression
            ? ParseNodeKind::Call
            : ParseNodeKind::New;
        result->setKind(pnk);
        result->prepend(callee);

        break;
      }
      case BinKind::SequenceExpression: {
        for (auto field : fields) {
            switch (field) {
              case BinField::Expressions:
                MOZ_TRY_VAR(result, parseExpressionList(/* acceptElisions = */ false));
                break;
              default:
                return raiseInvalidField("SequenceExpression", field);
            }
        }

        if (!result)
            return raiseMissingField("SequenceExpression", BinField::Expression);

        result->setKind(ParseNodeKind::Comma);
        break;
      }
      default:
        return raiseInvalidKind("Expression", kind);
    }

    return result;
}

JS::Result<ParseNode*>
BinASTParser::parseNumericLiteralAux(const BinKind kind, const BinFields& fields)
{
    auto start = tokenizer_->offset();

    Maybe<double> value;
    for (auto field : fields) {
        switch (field) {
          case BinField::Value:
            MOZ_TRY_EMPLACE(value, readNumber());
            break;
          default:
            return raiseInvalidField("NumericLiteral", field);
        }
    }

    // In case of absent optional fields, inject default values.
    if (!value)
        value.emplace(0);

    TRY_DECL(result, factory_.newNumber(*value, DecimalPoint::HasDecimal, tokenizer_->pos(start)));
    return result;
}

JS::Result<ParseNode*>
BinASTParser::parseStringLiteralAux(const BinKind kind, const BinFields& fields)
{
    auto start = tokenizer_->offset();

    RootedAtom value(cx_);
    for (auto field : fields) {
        switch (field) {
          case BinField::Value:
            MOZ_TRY(readString(&value));
            break;
          default:
            return raiseInvalidField("StringLiteral", field);
        }
    }

    if (!value)
        return raiseMissingField("StringLiteral", BinField::Value);

    TRY_DECL(result, factory_.newStringLiteral(value, tokenizer_->pos(start)));
    return result;
}

JS::Result<ParseNode*>
BinASTParser::parseArrayExpressionAux(const BinKind kind, const BinFields& fields)
{
    MOZ_ASSERT(kind == BinKind::ArrayExpression);

    ParseNode* result(nullptr);
    for (auto field : fields) {
        switch (field) {
          case BinField::Elements: {
            MOZ_TRY_VAR(result, parseExpressionList(/* acceptElisions = */ true));
            break;
          }
          default:
            return raiseInvalidField("ArrayExpression", field);
        }
    }

    // Inject default values for absent fields.
    if (!result)
        TRY_VAR(result, factory_.newArrayLiteral(tokenizer_->offset()));

    MOZ_ASSERT(result->isKind(ParseNodeKind::Array));
    return result;
}

JS::Result<ParseNode*>
BinASTParser::parseObjectExpressionAux(const BinKind kind, const BinFields& fields)
{
    MOZ_ASSERT(kind == BinKind::ObjectExpression);

    ParseNode* result(nullptr);
    for (auto field : fields) {
        switch (field) {
          case BinField::Properties: {
            MOZ_TRY_VAR(result, parseObjectMemberList());
            break;
          }
          default:
            return raiseInvalidField("Property | Method", field);
        }
    }

    if (!result)
        TRY_VAR(result, factory_.newObjectLiteral(tokenizer_->offset()));

    MOZ_ASSERT(result->isArity(PN_LIST));
    MOZ_ASSERT(result->isKind(ParseNodeKind::Object));

#if defined(DEBUG)
    // Sanity check.
    for (ParseNode* iter = result->pn_head; iter != nullptr; iter = iter->pn_next) {
        MOZ_ASSERT(iter->isKind(ParseNodeKind::Colon));
        MOZ_ASSERT(iter->pn_left != nullptr);
        MOZ_ASSERT(iter->pn_right != nullptr);
    }
#endif // defined(DEBUG)

    return result;
}

JS::Result<ParseNode*>
BinASTParser::parseMemberExpressionAux(const BinKind kind, const BinFields& fields)
{
    MOZ_ASSERT(kind == BinKind::DotExpression || kind == BinKind::BracketExpression);

    ParseNode* object(nullptr);
    ParseNode* property(nullptr);

    for (auto field : fields) {
        switch (field) {
          case BinField::Object:
            MOZ_TRY_VAR(object, parseExpression());
            break;
          case BinField::Property:
            if (kind == BinKind::BracketExpression)
                MOZ_TRY_VAR(property, parseExpression());
            else
                MOZ_TRY_VAR(property, parseIdentifier());
            break;
          default:
            return raiseInvalidField("MemberExpression", field);
        }
    }

    // In case of absent required fields, fail.
    if (!object)
        return raiseMissingField("MemberExpression", BinField::Object);
    if (!property)
        return raiseMissingField("MemberExpression", BinField::Property);

    ParseNode* result(nullptr);
    if (kind == BinKind::DotExpression) {
        MOZ_ASSERT(property->isKind(ParseNodeKind::Name));
        PropertyName* name = property->pn_atom->asPropertyName();
        TRY_VAR(result, factory_.newPropertyAccess(object, name, tokenizer_->offset()));
    } else {
        TRY_VAR(result, factory_.newPropertyByValue(object, property, tokenizer_->offset()));
    }

    return result;
}

JS::Result<ParseNode*>
BinASTParser::parseDirectiveList()
{
    uint32_t length;
    AutoList guard(*tokenizer_);
    TRY(tokenizer_->enterList(length, guard));

    TokenPos pos = tokenizer_->pos();
    TRY_DECL(result, factory_.newStatementList(pos));

    RootedAtom value(cx_);
    for (uint32_t i = 0; i < length; ++i) {
        value = nullptr;
        MOZ_TRY(readString(&value));

        TRY_DECL(directive, factory_.newStringLiteral(value, pos));
        factory_.addStatementToList(result, directive);
    }

    TRY(guard.done());
    return result;
}

JS::Result<ParseNode*>
BinASTParser::parseSwitchCase()
{
    const size_t start = tokenizer_->offset();

    BinKind kind;
    BinFields fields(cx_);
    AutoTaggedTuple guard(*tokenizer_);

    TRY(tokenizer_->enterTaggedTuple(kind, fields, guard));
    if (kind != BinKind::SwitchCase)
        return raiseInvalidKind("SwitchCase", kind);

    ParseNode* test(nullptr); // Optional.
    ParseNode* statements(nullptr); // Required.

    for (auto field : fields) {
        switch (field) {
          case BinField::Test:
            MOZ_TRY_VAR(test, parseExpression());
            break;
          case BinField::Consequent:
            MOZ_TRY_VAR(statements, parseStatementList());
            break;
          default:
            return raiseInvalidField("SwitchCase", field);
        }
    }

    TRY(guard.done());
    if (!statements)
        return raiseMissingField("SwitchCase", BinField::Consequent);

    MOZ_ASSERT(statements->isKind(ParseNodeKind::StatementList));

    TRY_DECL(result, factory_.newCaseOrDefault(start, test, statements));

    return result;
}

JS::Result<ParseNode*>
BinASTParser::parseCatchClause()
{
    ParseContext::Statement stmt(parseContext_, StatementKind::Catch);
    ParseContext::Scope scope(cx_, parseContext_, usedNames_);
    TRY(scope.init(parseContext_));

    BinKind kind;
    BinFields fields(cx_);
    AutoTaggedTuple guard(*tokenizer_);

    TRY(tokenizer_->enterTaggedTuple(kind, fields, guard));
    ParseNode* result(nullptr);

    switch (kind) {
      default:
        return raiseInvalidKind("CatchClause", kind);
      case BinKind::CatchClause: {
        ParseNode* param(nullptr);
        ParseNode* body(nullptr);

        for (auto field : fields) {
            switch (field) {
              case BinField::Param:
                MOZ_TRY_VAR(param, parsePattern());
                break;
              case BinField::Body:
                MOZ_TRY_VAR(body, parseBlockStatement());
                break;
              case BinField::BINJS_Scope:
                MOZ_TRY(parseAndUpdateCurrentScope());
                break;
              default:
                return raiseInvalidField("CatchClause", field);
            }
        }

        if (!param)
            return raiseMissingField("CatchClause", BinField::Param);
        if (!body)
            return raiseMissingField("CatchClause", BinField::Body);

        TRY_DECL(bindings, NewLexicalScopeData(cx_, scope, alloc_, parseContext_));
        TRY_VAR(result, factory_.newLexicalScope(*bindings, body));
        TRY(factory_.setupCatchScope(result, param, body));
      }
    }

    TRY(guard.done());
    return result;
}

JS::Result<ParseNode*>
BinASTParser::parseArgumentList()
{
    uint32_t length;
    AutoList guard(*tokenizer_);

    TRY(tokenizer_->enterList(length, guard));
    ParseNode* result = new_<ListNode>(ParseNodeKind::ParamsBody, tokenizer_->pos());

    for (uint32_t i = 0; i < length; ++i) {
        ParseNode* pattern;
        MOZ_TRY_VAR(pattern, parsePattern());

        result->appendWithoutOrderAssumption(pattern);
    }

    TRY(guard.done());
    return result;
}

JS::Result<ParseNode*>
BinASTParser::parseIdentifier()
{
    BinKind kind;
    BinFields fields(cx_);
    AutoTaggedTuple guard(*tokenizer_);

    TRY(tokenizer_->enterTaggedTuple(kind, fields, guard));
    ParseNode* result;
    MOZ_TRY_VAR(result, parseIdentifierAux(kind, fields));

    TRY(guard.done());
    return result;
}

JS::Result<ParseNode*>
BinASTParser::parseIdentifierAux(const BinKind, const BinFields& fields, const bool expectObjectPropertyName /* = false */)
{
    const size_t start = tokenizer_->offset();

    RootedAtom id(cx_);
    for (auto field : fields) {
        switch (field) {
          case BinField::Name:
            MOZ_TRY(readString(&id));
            break;
          default:
            return raiseInvalidField("Identifier", field);
        }
    }

    if (!id)
        return raiseMissingField("Identifier", BinField::Name);

    if (!IsIdentifier(id))
        return raiseError("Invalid identifier");
    if (!expectObjectPropertyName && IsKeyword(id))
        return raiseError("Invalid identifier (keyword)");

    // Once `IsIdentifier` has returned true, we may call `asPropertyName()` without fear.
    TokenPos pos = tokenizer_->pos(start);

    ParseNode* result;
    if (expectObjectPropertyName)
        TRY_VAR(result, factory_.newObjectLiteralPropertyName(id->asPropertyName(), pos));
    else
        TRY_VAR(result, factory_.newName(id->asPropertyName(), pos, cx_));

    return result;
}


JS::Result<ParseNode*>
BinASTParser::parsePattern()
{
    BinKind kind;
    BinFields fields(cx_);
    AutoTaggedTuple guard(*tokenizer_);

    TRY(tokenizer_->enterTaggedTuple(kind, fields, guard));
    ParseNode* result;
    MOZ_TRY_VAR(result, parsePatternAux(kind, fields));

    TRY(guard.done());
    return result;
}

JS::Result<ParseNode*>
BinASTParser::parsePatternAux(const BinKind kind, const BinFields& fields)
{
    ParseNode* result;
    switch (kind) {
      case BinKind::Identifier:
        MOZ_TRY_VAR(result, parseIdentifierAux(kind ,fields));
        break;
      default:
        return raiseInvalidKind("Pattern", kind);
    }

    return result;
}

JS::Result<ParseNode*>
BinASTParser::parseObjectMember()
{
    BinKind kind;
    BinFields fields(cx_);
    AutoTaggedTuple guard(*tokenizer_);

    TRY(tokenizer_->enterTaggedTuple(kind, fields, guard));
    ParseNode* result(nullptr);

    switch (kind) {
      case BinKind::ObjectProperty: {
        ParseNode* key(nullptr);
        ParseNode* value(nullptr);
        for (auto field : fields) {
            switch (field) {
              case BinField::Key:
                MOZ_TRY_VAR(key, parseObjectPropertyName());
                break;
              case BinField::Value:
                MOZ_TRY_VAR(value, parseExpression());
                break;
              default:
                return raiseInvalidField("ObjectMember", field);
            }
        }

        if (!key)
            return raiseMissingField("ObjectMember", BinField::Key);
        if (!value)
            return raiseMissingField("ObjectMember", BinField::Value);

        if (!factory_.isUsableAsObjectPropertyName(key))
            return raiseError("ObjectMember key kind");

        TRY_VAR(result, factory_.newObjectMethodOrPropertyDefinition(key, value, AccessorType::None));

        break;
      }
      case BinKind::ObjectMethod:
      case BinKind::ObjectGetter:
      case BinKind::ObjectSetter:
        MOZ_TRY_VAR(result, parseFunctionAux(kind, fields));

        if (!result)
            return raiseEmpty("ObjectMethod");

        MOZ_ASSERT(result->isKind(ParseNodeKind::Colon));
        break;
      default:
        return raiseInvalidKind("ObjectMember", kind);
    }

    TRY(guard.done());
    MOZ_ASSERT(result);
    return result;
}

JS::Result<ParseNode*>
BinASTParser::parseObjectMemberList()
{
    uint32_t length;
    AutoList guard(*tokenizer_);

    auto start = tokenizer_->offset();
    TRY(tokenizer_->enterList(length, guard));

    TRY_DECL(result, factory_.newObjectLiteral(start));

    for (uint32_t i = 0; i < length; ++i) {
        ParseNode* keyValue;
        MOZ_TRY_VAR(keyValue, parseObjectMember());
        MOZ_ASSERT(keyValue);

        result->appendWithoutOrderAssumption(keyValue);
    }

    TRY(guard.done());
    return result;
}


JS::Result<Ok>
BinASTParser::checkEmptyTuple(const BinKind kind, const BinFields& fields)
{
    if (fields.length() != 0)
        return raiseInvalidField(describeBinKind(kind), fields[0]);

    return Ok();
}


JS::Result<Ok>
BinASTParser::readString(MutableHandleAtom out)
{
    MOZ_ASSERT(!out);

    Maybe<Chars> string;
    MOZ_TRY(readString(string));
    MOZ_ASSERT(string);

    RootedAtom atom(cx_);
    TRY_VAR(atom, Atomize(cx_, (const char*)string->begin(), string->length()));

    out.set(Move(atom));
    return Ok();
}

JS::Result<ParseNode*>
BinASTParser::parsePropertyName()
{
    RootedAtom atom(cx_);
    MOZ_TRY(readString(&atom));

    TokenPos pos = tokenizer_->pos();

    ParseNode* result;

    // If the atom matches an index (e.g. "3"), we need to normalize the
    // propertyName to ensure that it has the same representation as
    // the numeric index (e.g. 3).
    uint32_t index;
    if (atom->isIndex(&index))
        TRY_VAR(result, factory_.newNumber(index, NoDecimal, pos));
    else
        TRY_VAR(result, factory_.newStringLiteral(atom, pos));

    return result;
}

JS::Result<Ok>
BinASTParser::readString(Maybe<Chars>& out)
{
    MOZ_ASSERT(out.isNothing());
    Chars result(cx_);
    TRY(tokenizer_->readChars(result));

    out.emplace(Move(result));
    return Ok();
}

JS::Result<double>
BinASTParser::readNumber()
{
    double result;
    TRY(tokenizer_->readDouble(result));

    return result;
}

JS::Result<bool>
BinASTParser::readBool()
{
    bool result;
    TRY(tokenizer_->readBool(result));

    return result;
}

mozilla::GenericErrorResult<JS::Error&>
BinASTParser::raiseInvalidKind(const char* superKind, const BinKind kind)
{
    Sprinter out(cx_);
    TRY(out.init());
    TRY(out.printf("In %s, invalid kind %s", superKind, describeBinKind(kind)));
    return raiseError(out.string());
}

mozilla::GenericErrorResult<JS::Error&>
BinASTParser::raiseInvalidField(const char* kind, const BinField field)
{
    Sprinter out(cx_);
    TRY(out.init());
    TRY(out.printf("In %s, invalid field '%s'", kind, describeBinField(field)));
    return raiseError(out.string());
}

mozilla::GenericErrorResult<JS::Error&>
BinASTParser::raiseInvalidEnum(const char* kind, const Chars& value)
{
    // We don't trust the actual chars of `value` to be properly formatted anything, so let's not use
    // them anywhere.
    return raiseError("Invalid enum");
}

mozilla::GenericErrorResult<JS::Error&>
BinASTParser::raiseMissingField(const char* kind, const BinField field)
{
    Sprinter out(cx_);
    TRY(out.init());
    TRY(out.printf("In %s, missing field '%s'", kind, describeBinField(field)));

    return raiseError(out.string());
}

mozilla::GenericErrorResult<JS::Error&>
BinASTParser::raiseEmpty(const char* description)
{
    Sprinter out(cx_);
    TRY(out.init());
    TRY(out.printf("Empty %s", description));

    return raiseError(out.string());
}

mozilla::GenericErrorResult<JS::Error&>
BinASTParser::raiseOOM()
{
    ReportOutOfMemory(cx_);
    return cx_->alreadyReportedError();
}

mozilla::GenericErrorResult<JS::Error&>
BinASTParser::raiseError(BinKind kind, const char* description)
{
    Sprinter out(cx_);
    TRY(out.init());
    TRY(out.printf("In %s, ", description));
    MOZ_ALWAYS_FALSE(tokenizer_->raiseError(out.string()));

    return cx_->alreadyReportedError();
}

mozilla::GenericErrorResult<JS::Error&>
BinASTParser::raiseError(const char* description)
{
    MOZ_ALWAYS_FALSE(tokenizer_->raiseError(description));
    return cx_->alreadyReportedError();
}

void
BinASTParser::poison()
{
    tokenizer_.reset();
}

void
BinASTParser::reportErrorNoOffsetVA(unsigned errorNumber, va_list args)
{
    ErrorMetadata metadata;
    metadata.filename = getFilename();
    metadata.lineNumber = 0;
    metadata.columnNumber = offset();
    ReportCompileError(cx_, Move(metadata), nullptr, JSREPORT_ERROR, errorNumber, args);
}

bool
BinASTParser::hasUsedName(HandlePropertyName name)
{
    if (UsedNamePtr p = usedNames_.lookup(name))
        return p->value().isUsedInScript(parseContext_->scriptId());

    return false;
}

void
TraceBinParser(JSTracer* trc, AutoGCRooter* parser)
{
    static_cast<BinASTParser*>(parser)->trace(trc);
}

} // namespace frontend
} // namespace js


// #undef everything, to avoid collisions with unified builds.

#undef TRY
#undef TRY_VAR
#undef TRY_DECL
#undef TRY_EMPL
#undef MOZ_TRY_EMPLACE
