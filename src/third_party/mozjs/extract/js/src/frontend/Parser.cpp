/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * JS parser.
 *
 * This is a recursive-descent parser for the JavaScript language specified by
 * "The ECMAScript Language Specification" (Standard ECMA-262).  It uses
 * lexical and semantic feedback to disambiguate non-LL(1) structures.  It
 * generates trees of nodes induced by the recursive parsing (not precise
 * syntax trees, see Parser.h).  After tree construction, it rewrites trees to
 * fold constants and evaluate compile-time expressions.
 *
 * This parser attempts no error recovery.
 */

#include "frontend/Parser.h"

#include "mozilla/ArrayUtils.h"
#include "mozilla/Assertions.h"
#include "mozilla/Casting.h"
#include "mozilla/Range.h"
#include "mozilla/Sprintf.h"
#include "mozilla/Try.h"  // MOZ_TRY*
#include "mozilla/Utf8.h"
#include "mozilla/Variant.h"

#include <memory>
#include <new>
#include <type_traits>

#include "jsnum.h"
#include "jstypes.h"

#include "frontend/FoldConstants.h"
#include "frontend/FunctionSyntaxKind.h"  // FunctionSyntaxKind
#include "frontend/ModuleSharedContext.h"
#include "frontend/ParseNode.h"
#include "frontend/ParseNodeVerify.h"
#include "frontend/Parser-macros.h"  // MOZ_TRY_VAR_OR_RETURN
#include "frontend/ParserAtom.h"  // TaggedParserAtomIndex, ParserAtomsTable, ParserAtom
#include "frontend/ScriptIndex.h"  // ScriptIndex
#include "frontend/TokenStream.h"  // IsKeyword, ReservedWordTokenKind, ReservedWordToCharZ, DeprecatedContent, *TokenStream*, CharBuffer, TokenKindToDesc
#include "irregexp/RegExpAPI.h"
#include "js/ColumnNumber.h"  // JS::LimitedColumnNumberOneOrigin, JS::ColumnNumberOneOrigin
#include "js/ErrorReport.h"           // JSErrorBase
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/HashTable.h"
#include "js/RegExpFlags.h"     // JS::RegExpFlags
#include "js/Stack.h"           // JS::NativeStackLimit
#include "util/StringBuffer.h"  // StringBuffer
#include "vm/BytecodeUtil.h"
#include "vm/FunctionFlags.h"          // js::FunctionFlags
#include "vm/GeneratorAndAsyncKind.h"  // js::GeneratorKind, js::FunctionAsyncKind
#include "vm/JSContext.h"
#include "vm/JSScript.h"
#include "vm/ModuleBuilder.h"  // js::ModuleBuilder
#include "vm/Scope.h"          // GetScopeDataTrailingNames
#include "wasm/AsmJS.h"

#include "frontend/ParseContext-inl.h"
#include "frontend/SharedContext-inl.h"

using namespace js;

using mozilla::AssertedCast;
using mozilla::AsVariant;
using mozilla::Maybe;
using mozilla::Nothing;
using mozilla::PointerRangeSize;
using mozilla::Some;
using mozilla::Utf8Unit;

using JS::ReadOnlyCompileOptions;
using JS::RegExpFlags;

namespace js::frontend {

using DeclaredNamePtr = ParseContext::Scope::DeclaredNamePtr;
using AddDeclaredNamePtr = ParseContext::Scope::AddDeclaredNamePtr;
using BindingIter = ParseContext::Scope::BindingIter;
using UsedNamePtr = UsedNameTracker::UsedNameMap::Ptr;

using ParserBindingNameVector = Vector<ParserBindingName, 6>;

static inline void PropagateTransitiveParseFlags(const FunctionBox* inner,
                                                 SharedContext* outer) {
  if (inner->bindingsAccessedDynamically()) {
    outer->setBindingsAccessedDynamically();
  }
  if (inner->hasDirectEval()) {
    outer->setHasDirectEval();
  }
}

static bool StatementKindIsBraced(StatementKind kind) {
  return kind == StatementKind::Block || kind == StatementKind::Switch ||
         kind == StatementKind::Try || kind == StatementKind::Catch ||
         kind == StatementKind::Finally;
}

template <class ParseHandler, typename Unit>
inline typename GeneralParser<ParseHandler, Unit>::FinalParser*
GeneralParser<ParseHandler, Unit>::asFinalParser() {
  static_assert(
      std::is_base_of_v<GeneralParser<ParseHandler, Unit>, FinalParser>,
      "inheritance relationship required by the static_cast<> below");

  return static_cast<FinalParser*>(this);
}

template <class ParseHandler, typename Unit>
inline const typename GeneralParser<ParseHandler, Unit>::FinalParser*
GeneralParser<ParseHandler, Unit>::asFinalParser() const {
  static_assert(
      std::is_base_of_v<GeneralParser<ParseHandler, Unit>, FinalParser>,
      "inheritance relationship required by the static_cast<> below");

  return static_cast<const FinalParser*>(this);
}

template <class ParseHandler, typename Unit>
template <typename ConditionT, typename ErrorReportT>
bool GeneralParser<ParseHandler, Unit>::mustMatchTokenInternal(
    ConditionT condition, ErrorReportT errorReport) {
  MOZ_ASSERT(condition(TokenKind::Div) == false);
  MOZ_ASSERT(condition(TokenKind::DivAssign) == false);
  MOZ_ASSERT(condition(TokenKind::RegExp) == false);

  TokenKind actual;
  if (!tokenStream.getToken(&actual, TokenStream::SlashIsInvalid)) {
    return false;
  }
  if (!condition(actual)) {
    errorReport(actual);
    return false;
  }
  return true;
}

ParserSharedBase::ParserSharedBase(FrontendContext* fc,
                                   CompilationState& compilationState,
                                   Kind kind)
    : fc_(fc),
      alloc_(compilationState.parserAllocScope.alloc()),
      compilationState_(compilationState),
      pc_(nullptr),
      usedNames_(compilationState.usedNames) {
  fc_->nameCollectionPool().addActiveCompilation();
}

ParserSharedBase::~ParserSharedBase() {
  fc_->nameCollectionPool().removeActiveCompilation();
}

#if defined(DEBUG) || defined(JS_JITSPEW)
void ParserSharedBase::dumpAtom(TaggedParserAtomIndex index) const {
  parserAtoms().dump(index);
}
#endif

ParserBase::ParserBase(FrontendContext* fc,
                       const ReadOnlyCompileOptions& options,
                       bool foldConstants, CompilationState& compilationState)
    : ParserSharedBase(fc, compilationState, ParserSharedBase::Kind::Parser),
      anyChars(fc, options, this),
      ss(nullptr),
      foldConstants_(foldConstants),
#ifdef DEBUG
      checkOptionsCalled_(false),
#endif
      isUnexpectedEOF_(false),
      awaitHandling_(AwaitIsName),
      inParametersOfAsyncFunction_(false) {
}

bool ParserBase::checkOptions() {
#ifdef DEBUG
  checkOptionsCalled_ = true;
#endif

  return anyChars.checkOptions();
}

ParserBase::~ParserBase() { MOZ_ASSERT(checkOptionsCalled_); }

JSAtom* ParserBase::liftParserAtomToJSAtom(TaggedParserAtomIndex index) {
  JSContext* cx = fc_->maybeCurrentJSContext();
  MOZ_ASSERT(cx);
  return parserAtoms().toJSAtom(cx, fc_, index,
                                compilationState_.input.atomCache);
}

template <class ParseHandler>
PerHandlerParser<ParseHandler>::PerHandlerParser(
    FrontendContext* fc, const ReadOnlyCompileOptions& options,
    bool foldConstants, CompilationState& compilationState,
    void* internalSyntaxParser)
    : ParserBase(fc, options, foldConstants, compilationState),
      handler_(fc, compilationState),
      internalSyntaxParser_(internalSyntaxParser) {
  MOZ_ASSERT(compilationState.isInitialStencil() ==
             compilationState.input.isInitialStencil());
}

template <class ParseHandler, typename Unit>
GeneralParser<ParseHandler, Unit>::GeneralParser(
    FrontendContext* fc, const ReadOnlyCompileOptions& options,
    const Unit* units, size_t length, bool foldConstants,
    CompilationState& compilationState, SyntaxParser* syntaxParser)
    : Base(fc, options, foldConstants, compilationState, syntaxParser),
      tokenStream(fc, &compilationState.parserAtoms, options, units, length) {}

template <typename Unit>
void Parser<SyntaxParseHandler, Unit>::setAwaitHandling(
    AwaitHandling awaitHandling) {
  this->awaitHandling_ = awaitHandling;
}

template <typename Unit>
void Parser<FullParseHandler, Unit>::setAwaitHandling(
    AwaitHandling awaitHandling) {
  this->awaitHandling_ = awaitHandling;
  if (SyntaxParser* syntaxParser = getSyntaxParser()) {
    syntaxParser->setAwaitHandling(awaitHandling);
  }
}

template <class ParseHandler, typename Unit>
inline void GeneralParser<ParseHandler, Unit>::setAwaitHandling(
    AwaitHandling awaitHandling) {
  asFinalParser()->setAwaitHandling(awaitHandling);
}

template <typename Unit>
void Parser<SyntaxParseHandler, Unit>::setInParametersOfAsyncFunction(
    bool inParameters) {
  this->inParametersOfAsyncFunction_ = inParameters;
}

template <typename Unit>
void Parser<FullParseHandler, Unit>::setInParametersOfAsyncFunction(
    bool inParameters) {
  this->inParametersOfAsyncFunction_ = inParameters;
  if (SyntaxParser* syntaxParser = getSyntaxParser()) {
    syntaxParser->setInParametersOfAsyncFunction(inParameters);
  }
}

template <class ParseHandler, typename Unit>
inline void GeneralParser<ParseHandler, Unit>::setInParametersOfAsyncFunction(
    bool inParameters) {
  asFinalParser()->setInParametersOfAsyncFunction(inParameters);
}

template <class ParseHandler>
FunctionBox* PerHandlerParser<ParseHandler>::newFunctionBox(
    FunctionNodeType funNode, TaggedParserAtomIndex explicitName,
    FunctionFlags flags, uint32_t toStringStart, Directives inheritedDirectives,
    GeneratorKind generatorKind, FunctionAsyncKind asyncKind) {
  MOZ_ASSERT(funNode);

  ScriptIndex index = ScriptIndex(compilationState_.scriptData.length());
  if (uint32_t(index) >= TaggedScriptThingIndex::IndexLimit) {
    ReportAllocationOverflow(fc_);
    return nullptr;
  }
  if (!compilationState_.appendScriptStencilAndData(fc_)) {
    return nullptr;
  }

  bool isInitialStencil = compilationState_.isInitialStencil();

  // This source extent will be further filled in during the remainder of parse.
  SourceExtent extent;
  extent.toStringStart = toStringStart;

  FunctionBox* funbox = alloc_.new_<FunctionBox>(
      fc_, extent, compilationState_, inheritedDirectives, generatorKind,
      asyncKind, isInitialStencil, explicitName, flags, index);
  if (!funbox) {
    ReportOutOfMemory(fc_);
    return nullptr;
  }

  handler_.setFunctionBox(funNode, funbox);

  return funbox;
}

template <class ParseHandler>
FunctionBox* PerHandlerParser<ParseHandler>::newFunctionBox(
    FunctionNodeType funNode, const ScriptStencil& cachedScriptData,
    const ScriptStencilExtra& cachedScriptExtra) {
  MOZ_ASSERT(funNode);

  ScriptIndex index = ScriptIndex(compilationState_.scriptData.length());
  if (uint32_t(index) >= TaggedScriptThingIndex::IndexLimit) {
    ReportAllocationOverflow(fc_);
    return nullptr;
  }
  if (!compilationState_.appendScriptStencilAndData(fc_)) {
    return nullptr;
  }

  FunctionBox* funbox = alloc_.new_<FunctionBox>(
      fc_, cachedScriptExtra.extent, compilationState_,
      Directives(/* strict = */ false), cachedScriptExtra.generatorKind(),
      cachedScriptExtra.asyncKind(), compilationState_.isInitialStencil(),
      cachedScriptData.functionAtom, cachedScriptData.functionFlags, index);
  if (!funbox) {
    ReportOutOfMemory(fc_);
    return nullptr;
  }

  handler_.setFunctionBox(funNode, funbox);
  funbox->initFromScriptStencilExtra(cachedScriptExtra);

  return funbox;
}

bool ParserBase::setSourceMapInfo() {
  // If support for source pragmas have been fully disabled, we can skip
  // processing of all of these values.
  if (!options().sourcePragmas()) {
    return true;
  }

  // Not all clients initialize ss. Can't update info to an object that isn't
  // there.
  if (!ss) {
    return true;
  }

  if (anyChars.hasDisplayURL()) {
    if (!ss->setDisplayURL(fc_, anyChars.displayURL())) {
      return false;
    }
  }

  if (anyChars.hasSourceMapURL()) {
    MOZ_ASSERT(!ss->hasSourceMapURL());
    if (!ss->setSourceMapURL(fc_, anyChars.sourceMapURL())) {
      return false;
    }
  }

  /*
   * Source map URLs passed as a compile option (usually via a HTTP source map
   * header) override any source map urls passed as comment pragmas.
   */
  if (options().sourceMapURL()) {
    // Warn about the replacement, but use the new one.
    if (ss->hasSourceMapURL()) {
      if (!warningNoOffset(JSMSG_ALREADY_HAS_PRAGMA, ss->filename(),
                           "//# sourceMappingURL")) {
        return false;
      }
    }

    if (!ss->setSourceMapURL(fc_, options().sourceMapURL())) {
      return false;
    }
  }

  return true;
}

/*
 * Parse a top-level JS script.
 */
template <class ParseHandler, typename Unit>
typename ParseHandler::ListNodeResult
GeneralParser<ParseHandler, Unit>::parse() {
  MOZ_ASSERT(checkOptionsCalled_);

  SourceExtent extent = SourceExtent::makeGlobalExtent(
      /* len = */ 0, options().lineno,
      JS::LimitedColumnNumberOneOrigin::fromUnlimited(
          JS::ColumnNumberOneOrigin(options().column)));
  Directives directives(options().forceStrictMode());
  GlobalSharedContext globalsc(this->fc_, ScopeKind::Global, options(),
                               directives, extent);
  SourceParseContext globalpc(this, &globalsc, /* newDirectives = */ nullptr);
  if (!globalpc.init()) {
    return errorResult();
  }

  ParseContext::VarScope varScope(this);
  if (!varScope.init(pc_)) {
    return errorResult();
  }

  ListNodeType stmtList;
  MOZ_TRY_VAR(stmtList, statementList(YieldIsName));

  TokenKind tt;
  if (!tokenStream.getToken(&tt, TokenStream::SlashIsRegExp)) {
    return errorResult();
  }
  if (tt != TokenKind::Eof) {
    error(JSMSG_GARBAGE_AFTER_INPUT, "script", TokenKindToDesc(tt));
    return errorResult();
  }

  if (!CheckParseTree(this->fc_, alloc_, stmtList)) {
    return errorResult();
  }

  if (foldConstants_) {
    Node node = stmtList;
    // Don't constant-fold inside "use asm" code, as this could create a parse
    // tree that doesn't type-check as asm.js.
    if (!pc_->useAsmOrInsideUseAsm()) {
      if (!FoldConstants(this->fc_, this->parserAtoms(), &node, &handler_)) {
        return errorResult();
      }
    }
    stmtList = handler_.asListNode(node);
  }

  return stmtList;
}

/*
 * Strict mode forbids introducing new definitions for 'eval', 'arguments',
 * 'let', 'static', 'yield', or for any strict mode reserved word.
 */
bool ParserBase::isValidStrictBinding(TaggedParserAtomIndex name) {
  TokenKind tt = ReservedWordTokenKind(name);
  if (tt == TokenKind::Limit) {
    return name != TaggedParserAtomIndex::WellKnown::eval() &&
           name != TaggedParserAtomIndex::WellKnown::arguments();
  }
  return tt != TokenKind::Let && tt != TokenKind::Static &&
         tt != TokenKind::Yield && !TokenKindIsStrictReservedWord(tt);
}

/*
 * Returns true if all parameter names are valid strict mode binding names and
 * no duplicate parameter names are present.
 */
bool ParserBase::hasValidSimpleStrictParameterNames() {
  MOZ_ASSERT(pc_->isFunctionBox() &&
             pc_->functionBox()->hasSimpleParameterList());

  if (pc_->functionBox()->hasDuplicateParameters) {
    return false;
  }

  for (auto name : pc_->positionalFormalParameterNames()) {
    MOZ_ASSERT(name);
    if (!isValidStrictBinding(name)) {
      return false;
    }
  }
  return true;
}

template <class ParseHandler, typename Unit>
void GeneralParser<ParseHandler, Unit>::reportMissingClosing(
    unsigned errorNumber, unsigned noteNumber, uint32_t openedPos) {
  auto notes = MakeUnique<JSErrorNotes>();
  if (!notes) {
    ReportOutOfMemory(this->fc_);
    return;
  }

  uint32_t line;
  JS::LimitedColumnNumberOneOrigin column;
  tokenStream.computeLineAndColumn(openedPos, &line, &column);

  const size_t MaxWidth = sizeof("4294967295");
  char columnNumber[MaxWidth];
  SprintfLiteral(columnNumber, "%" PRIu32, column.oneOriginValue());
  char lineNumber[MaxWidth];
  SprintfLiteral(lineNumber, "%" PRIu32, line);

  if (!notes->addNoteASCII(this->fc_, getFilename().c_str(), 0, line,
                           JS::ColumnNumberOneOrigin(column), GetErrorMessage,
                           nullptr, noteNumber, lineNumber, columnNumber)) {
    return;
  }

  errorWithNotes(std::move(notes), errorNumber);
}

template <class ParseHandler, typename Unit>
void GeneralParser<ParseHandler, Unit>::reportRedeclarationHelper(
    TaggedParserAtomIndex& name, DeclarationKind& prevKind, TokenPos& pos,
    uint32_t& prevPos, const unsigned& errorNumber,
    const unsigned& noteErrorNumber) {
  UniqueChars bytes = this->parserAtoms().toPrintableString(name);
  if (!bytes) {
    ReportOutOfMemory(this->fc_);
    return;
  }

  if (prevPos == DeclaredNameInfo::npos) {
    errorAt(pos.begin, errorNumber, DeclarationKindString(prevKind),
            bytes.get());
    return;
  }

  auto notes = MakeUnique<JSErrorNotes>();
  if (!notes) {
    ReportOutOfMemory(this->fc_);
    return;
  }

  uint32_t line;
  JS::LimitedColumnNumberOneOrigin column;
  tokenStream.computeLineAndColumn(prevPos, &line, &column);

  const size_t MaxWidth = sizeof("4294967295");
  char columnNumber[MaxWidth];
  SprintfLiteral(columnNumber, "%" PRIu32, column.oneOriginValue());
  char lineNumber[MaxWidth];
  SprintfLiteral(lineNumber, "%" PRIu32, line);

  if (!notes->addNoteASCII(this->fc_, getFilename().c_str(), 0, line,
                           JS::ColumnNumberOneOrigin(column), GetErrorMessage,
                           nullptr, noteErrorNumber, lineNumber,
                           columnNumber)) {
    return;
  }

  errorWithNotesAt(std::move(notes), pos.begin, errorNumber,
                   DeclarationKindString(prevKind), bytes.get());
}

template <class ParseHandler, typename Unit>
void GeneralParser<ParseHandler, Unit>::reportRedeclaration(
    TaggedParserAtomIndex name, DeclarationKind prevKind, TokenPos pos,
    uint32_t prevPos) {
  reportRedeclarationHelper(name, prevKind, pos, prevPos, JSMSG_REDECLARED_VAR,
                            JSMSG_PREV_DECLARATION);
}

template <class ParseHandler, typename Unit>
void GeneralParser<ParseHandler, Unit>::reportMismatchedPlacement(
    TaggedParserAtomIndex name, DeclarationKind prevKind, TokenPos pos,
    uint32_t prevPos) {
  reportRedeclarationHelper(name, prevKind, pos, prevPos,
                            JSMSG_MISMATCHED_PLACEMENT, JSMSG_PREV_DECLARATION);
}

// notePositionalFormalParameter is called for both the arguments of a regular
// function definition and the arguments specified by the Function
// constructor.
//
// The 'disallowDuplicateParams' bool indicates whether the use of another
// feature (destructuring or default arguments) disables duplicate arguments.
// (ECMA-262 requires us to support duplicate parameter names, but, for newer
// features, we consider the code to have "opted in" to higher standards and
// forbid duplicates.)
template <class ParseHandler, typename Unit>
bool GeneralParser<ParseHandler, Unit>::notePositionalFormalParameter(
    FunctionNodeType funNode, TaggedParserAtomIndex name, uint32_t beginPos,
    bool disallowDuplicateParams, bool* duplicatedParam) {
  if (AddDeclaredNamePtr p =
          pc_->functionScope().lookupDeclaredNameForAdd(name)) {
    if (disallowDuplicateParams) {
      error(JSMSG_BAD_DUP_ARGS);
      return false;
    }

    // Strict-mode disallows duplicate args. We may not know whether we are
    // in strict mode or not (since the function body hasn't been parsed).
    // In such cases, report will queue up the potential error and return
    // 'true'.
    if (pc_->sc()->strict()) {
      UniqueChars bytes = this->parserAtoms().toPrintableString(name);
      if (!bytes) {
        ReportOutOfMemory(this->fc_);
        return false;
      }
      if (!strictModeError(JSMSG_DUPLICATE_FORMAL, bytes.get())) {
        return false;
      }
    }

    *duplicatedParam = true;
  } else {
    DeclarationKind kind = DeclarationKind::PositionalFormalParameter;
    if (!pc_->functionScope().addDeclaredName(pc_, p, name, kind, beginPos)) {
      return false;
    }
  }

  if (!pc_->positionalFormalParameterNames().append(
          TrivialTaggedParserAtomIndex::from(name))) {
    ReportOutOfMemory(this->fc_);
    return false;
  }

  NameNodeType paramNode;
  MOZ_TRY_VAR_OR_RETURN(paramNode, newName(name), false);

  handler_.addFunctionFormalParameter(funNode, paramNode);
  return true;
}

template <class ParseHandler>
bool PerHandlerParser<ParseHandler>::noteDestructuredPositionalFormalParameter(
    FunctionNodeType funNode, Node destruct) {
  // Append an empty name to the positional formals vector to keep track of
  // argument slots when making FunctionScope::ParserData.
  if (!pc_->positionalFormalParameterNames().append(
          TrivialTaggedParserAtomIndex::null())) {
    ReportOutOfMemory(fc_);
    return false;
  }

  handler_.addFunctionFormalParameter(funNode, destruct);
  return true;
}

template <class ParseHandler, typename Unit>
bool GeneralParser<ParseHandler, Unit>::noteDeclaredName(
    TaggedParserAtomIndex name, DeclarationKind kind, TokenPos pos,
    ClosedOver isClosedOver) {
  // The asm.js validator does all its own symbol-table management so, as an
  // optimization, avoid doing any work here.
  if (pc_->useAsmOrInsideUseAsm()) {
    return true;
  }

  switch (kind) {
    case DeclarationKind::Var:
    case DeclarationKind::BodyLevelFunction: {
      Maybe<DeclarationKind> redeclaredKind;
      uint32_t prevPos;
      if (!pc_->tryDeclareVar(name, this, kind, pos.begin, &redeclaredKind,
                              &prevPos)) {
        return false;
      }

      if (redeclaredKind) {
        reportRedeclaration(name, *redeclaredKind, pos, prevPos);
        return false;
      }

      break;
    }

    case DeclarationKind::ModuleBodyLevelFunction: {
      MOZ_ASSERT(pc_->atModuleLevel());

      AddDeclaredNamePtr p = pc_->varScope().lookupDeclaredNameForAdd(name);
      if (p) {
        reportRedeclaration(name, p->value()->kind(), pos, p->value()->pos());
        return false;
      }

      if (!pc_->varScope().addDeclaredName(pc_, p, name, kind, pos.begin,
                                           isClosedOver)) {
        return false;
      }

      // Body-level functions in modules are always closed over.
      pc_->varScope().lookupDeclaredName(name)->value()->setClosedOver();

      break;
    }

    case DeclarationKind::FormalParameter: {
      // It is an early error if any non-positional formal parameter name
      // (e.g., destructuring formal parameter) is duplicated.

      AddDeclaredNamePtr p =
          pc_->functionScope().lookupDeclaredNameForAdd(name);
      if (p) {
        error(JSMSG_BAD_DUP_ARGS);
        return false;
      }

      if (!pc_->functionScope().addDeclaredName(pc_, p, name, kind, pos.begin,
                                                isClosedOver)) {
        return false;
      }

      break;
    }

    case DeclarationKind::LexicalFunction:
    case DeclarationKind::PrivateName:
    case DeclarationKind::Synthetic:
    case DeclarationKind::PrivateMethod: {
      ParseContext::Scope* scope = pc_->innermostScope();
      AddDeclaredNamePtr p = scope->lookupDeclaredNameForAdd(name);
      if (p) {
        reportRedeclaration(name, p->value()->kind(), pos, p->value()->pos());
        return false;
      }

      if (!scope->addDeclaredName(pc_, p, name, kind, pos.begin,
                                  isClosedOver)) {
        return false;
      }

      break;
    }

    case DeclarationKind::SloppyLexicalFunction: {
      // Functions in block have complex allowances in sloppy mode for being
      // labelled that other lexical declarations do not have. Those checks
      // are done in functionStmt.

      ParseContext::Scope* scope = pc_->innermostScope();
      if (AddDeclaredNamePtr p = scope->lookupDeclaredNameForAdd(name)) {
        // It is usually an early error if there is another declaration
        // with the same name in the same scope.
        //
        // Sloppy lexical functions may redeclare other sloppy lexical
        // functions for web compatibility reasons.
        if (p->value()->kind() != DeclarationKind::SloppyLexicalFunction) {
          reportRedeclaration(name, p->value()->kind(), pos, p->value()->pos());
          return false;
        }
      } else {
        if (!scope->addDeclaredName(pc_, p, name, kind, pos.begin,
                                    isClosedOver)) {
          return false;
        }
      }

      break;
    }

    case DeclarationKind::Let:
    case DeclarationKind::Const:
#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
    case DeclarationKind::Using:
    case DeclarationKind::AwaitUsing:
#endif
    case DeclarationKind::Class:
      // The BoundNames of LexicalDeclaration and ForDeclaration must not
      // contain 'let'. (CatchParameter is the only lexical binding form
      // without this restriction.)
      if (name == TaggedParserAtomIndex::WellKnown::let()) {
        errorAt(pos.begin, JSMSG_LEXICAL_DECL_DEFINES_LET);
        return false;
      }

      // For body-level lexically declared names in a function, it is an
      // early error if there is a formal parameter of the same name. This
      // needs a special check if there is an extra var scope due to
      // parameter expressions.
      if (pc_->isFunctionExtraBodyVarScopeInnermost()) {
        DeclaredNamePtr p = pc_->functionScope().lookupDeclaredName(name);
        if (p && DeclarationKindIsParameter(p->value()->kind())) {
          reportRedeclaration(name, p->value()->kind(), pos, p->value()->pos());
          return false;
        }
      }

      [[fallthrough]];

    case DeclarationKind::Import:
      // Module code is always strict, so 'let' is always a keyword and never a
      // name.
      MOZ_ASSERT(name != TaggedParserAtomIndex::WellKnown::let());
      [[fallthrough]];

    case DeclarationKind::SimpleCatchParameter:
    case DeclarationKind::CatchParameter: {
      ParseContext::Scope* scope = pc_->innermostScope();

      // It is an early error if there is another declaration with the same
      // name in the same scope.
      AddDeclaredNamePtr p = scope->lookupDeclaredNameForAdd(name);
      if (p) {
        reportRedeclaration(name, p->value()->kind(), pos, p->value()->pos());
        return false;
      }

      if (!scope->addDeclaredName(pc_, p, name, kind, pos.begin,
                                  isClosedOver)) {
        return false;
      }

      break;
    }

    case DeclarationKind::CoverArrowParameter:
      // CoverArrowParameter is only used as a placeholder declaration kind.
      break;

    case DeclarationKind::PositionalFormalParameter:
      MOZ_CRASH(
          "Positional formal parameter names should use "
          "notePositionalFormalParameter");
      break;

    case DeclarationKind::VarForAnnexBLexicalFunction:
      MOZ_CRASH(
          "Synthesized Annex B vars should go through "
          "addPossibleAnnexBFunctionBox, and "
          "propagateAndMarkAnnexBFunctionBoxes");
      break;
  }

  return true;
}

template <class ParseHandler, typename Unit>
bool GeneralParser<ParseHandler, Unit>::noteDeclaredPrivateName(
    Node nameNode, TaggedParserAtomIndex name, PropertyType propType,
    FieldPlacement placement, TokenPos pos) {
  ParseContext::Scope* scope = pc_->innermostScope();
  AddDeclaredNamePtr p = scope->lookupDeclaredNameForAdd(name);

  DeclarationKind declKind = DeclarationKind::PrivateName;

  // Our strategy for enabling debugger functionality is to mark names as closed
  // over, even if they don't necessarily need to be, to ensure that they are
  // included in the environment object. This allows us to easily look them up
  // by name when needed, even if there is no corresponding property on an
  // object, as is the case with getter, setters and private methods.
  ClosedOver closedOver = ClosedOver::Yes;
  PrivateNameKind kind;
  switch (propType) {
    case PropertyType::Field:
      kind = PrivateNameKind::Field;
      closedOver = ClosedOver::No;
      break;
    case PropertyType::FieldWithAccessor:
      // In this case, we create a new private field for the underlying storage,
      // and use the current name for the getter and setter.
      kind = PrivateNameKind::GetterSetter;
      break;
    case PropertyType::Method:
    case PropertyType::GeneratorMethod:
    case PropertyType::AsyncMethod:
    case PropertyType::AsyncGeneratorMethod:
      if (placement == FieldPlacement::Instance) {
        // Optimized private method. Non-optimized paths still get
        // DeclarationKind::Synthetic.
        declKind = DeclarationKind::PrivateMethod;
      }
      kind = PrivateNameKind::Method;
      break;
    case PropertyType::Getter:
      kind = PrivateNameKind::Getter;
      break;
    case PropertyType::Setter:
      kind = PrivateNameKind::Setter;
      break;
    default:
      MOZ_CRASH("Invalid Property Type for noteDeclarePrivateName");
  }

  if (p) {
    PrivateNameKind prevKind = p->value()->privateNameKind();
    if ((prevKind == PrivateNameKind::Getter &&
         kind == PrivateNameKind::Setter) ||
        (prevKind == PrivateNameKind::Setter &&
         kind == PrivateNameKind::Getter)) {
      // Private methods demands that
      //
      // class A {
      //   static set #x(_) {}
      //   get #x() { }
      // }
      //
      // Report a SyntaxError.
      if (placement == p->value()->placement()) {
        p->value()->setPrivateNameKind(PrivateNameKind::GetterSetter);
        handler_.setPrivateNameKind(nameNode, PrivateNameKind::GetterSetter);
        return true;
      }
    }

    reportMismatchedPlacement(name, p->value()->kind(), pos, p->value()->pos());
    return false;
  }

  if (!scope->addDeclaredName(pc_, p, name, declKind, pos.begin, closedOver)) {
    return false;
  }

  DeclaredNamePtr declared = scope->lookupDeclaredName(name);
  declared->value()->setPrivateNameKind(kind);
  declared->value()->setFieldPlacement(placement);
  handler_.setPrivateNameKind(nameNode, kind);

  return true;
}

bool ParserBase::noteUsedNameInternal(TaggedParserAtomIndex name,
                                      NameVisibility visibility,
                                      mozilla::Maybe<TokenPos> tokenPosition) {
  // The asm.js validator does all its own symbol-table management so, as an
  // optimization, avoid doing any work here.
  if (pc_->useAsmOrInsideUseAsm()) {
    return true;
  }

  // Global bindings are properties and not actual bindings; we don't need
  // to know if they are closed over. So no need to track used name at the
  // global scope. It is not incorrect to track them, this is an
  // optimization.
  //
  // Exceptions:
  //   (a) Track private name references, as the used names tracker is used to
  //       provide early errors for undeclared private name references
  //   (b) If the script has extra bindings, track all references to detect
  //       references to extra bindings
  ParseContext::Scope* scope = pc_->innermostScope();
  if (pc_->sc()->isGlobalContext() && scope == &pc_->varScope() &&
      visibility == NameVisibility::Public &&
      !this->compilationState_.input.hasExtraBindings()) {
    return true;
  }

  return usedNames_.noteUse(fc_, name, visibility, pc_->scriptId(), scope->id(),
                            tokenPosition);
}

template <class ParseHandler>
bool PerHandlerParser<ParseHandler>::
    propagateFreeNamesAndMarkClosedOverBindings(ParseContext::Scope& scope) {
  // Now that we have all the declared names in the scope, check which
  // functions should exhibit Annex B semantics.
  if (!scope.propagateAndMarkAnnexBFunctionBoxes(pc_, this)) {
    return false;
  }

  if (handler_.reuseClosedOverBindings()) {
    MOZ_ASSERT(pc_->isOutermostOfCurrentCompile());

    // Closed over bindings for all scopes are stored in a contiguous array, in
    // the same order as the order in which scopes are visited, and seprated by
    // TaggedParserAtomIndex::null().
    uint32_t slotCount = scope.declaredCount();
    while (auto parserAtom = handler_.nextLazyClosedOverBinding()) {
      scope.lookupDeclaredName(parserAtom)->value()->setClosedOver();
      MOZ_ASSERT(slotCount > 0);
      slotCount--;
    }

    if (pc_->isGeneratorOrAsync()) {
      scope.setOwnStackSlotCount(slotCount);
    }
    return true;
  }

  constexpr bool isSyntaxParser =
      std::is_same_v<ParseHandler, SyntaxParseHandler>;
  uint32_t scriptId = pc_->scriptId();
  uint32_t scopeId = scope.id();

  uint32_t slotCount = 0;
  for (BindingIter bi = scope.bindings(pc_); bi; bi++) {
    bool closedOver = false;
    if (UsedNamePtr p = usedNames_.lookup(bi.name())) {
      p->value().noteBoundInScope(scriptId, scopeId, &closedOver);
      if (closedOver) {
        bi.setClosedOver();

        if constexpr (isSyntaxParser) {
          if (!pc_->closedOverBindingsForLazy().append(
                  TrivialTaggedParserAtomIndex::from(bi.name()))) {
            ReportOutOfMemory(fc_);
            return false;
          }
        }
      }
    }

    if constexpr (!isSyntaxParser) {
      if (!closedOver) {
        slotCount++;
      }
    }
  }
  if constexpr (!isSyntaxParser) {
    if (pc_->isGeneratorOrAsync()) {
      scope.setOwnStackSlotCount(slotCount);
    }
  }

  // Append a nullptr to denote end-of-scope.
  if constexpr (isSyntaxParser) {
    if (!pc_->closedOverBindingsForLazy().append(
            TrivialTaggedParserAtomIndex::null())) {
      ReportOutOfMemory(fc_);
      return false;
    }
  }

  return true;
}

template <typename Unit>
bool Parser<FullParseHandler, Unit>::checkStatementsEOF() {
  // This is designed to be paired with parsing a statement list at the top
  // level.
  //
  // The statementList() call breaks on TokenKind::RightCurly, so make sure
  // we've reached EOF here.
  TokenKind tt;
  if (!tokenStream.peekToken(&tt, TokenStream::SlashIsRegExp)) {
    return false;
  }
  if (tt != TokenKind::Eof) {
    error(JSMSG_UNEXPECTED_TOKEN, "expression", TokenKindToDesc(tt));
    return false;
  }
  return true;
}

template <typename ScopeT>
typename ScopeT::ParserData* NewEmptyBindingData(FrontendContext* fc,
                                                 LifoAlloc& alloc,
                                                 uint32_t numBindings) {
  using Data = typename ScopeT::ParserData;
  size_t allocSize = SizeOfScopeData<Data>(numBindings);
  auto* bindings = alloc.newWithSize<Data>(allocSize, numBindings);
  if (!bindings) {
    ReportOutOfMemory(fc);
  }
  return bindings;
}

GlobalScope::ParserData* NewEmptyGlobalScopeData(FrontendContext* fc,
                                                 LifoAlloc& alloc,
                                                 uint32_t numBindings) {
  return NewEmptyBindingData<GlobalScope>(fc, alloc, numBindings);
}

LexicalScope::ParserData* NewEmptyLexicalScopeData(FrontendContext* fc,
                                                   LifoAlloc& alloc,
                                                   uint32_t numBindings) {
  return NewEmptyBindingData<LexicalScope>(fc, alloc, numBindings);
}

FunctionScope::ParserData* NewEmptyFunctionScopeData(FrontendContext* fc,
                                                     LifoAlloc& alloc,
                                                     uint32_t numBindings) {
  return NewEmptyBindingData<FunctionScope>(fc, alloc, numBindings);
}

namespace detail {

template <class SlotInfo>
static MOZ_ALWAYS_INLINE ParserBindingName* InitializeIndexedBindings(
    SlotInfo& slotInfo, ParserBindingName* start, ParserBindingName* cursor) {
  return cursor;
}

template <class SlotInfo, typename UnsignedInteger, typename... Step>
static MOZ_ALWAYS_INLINE ParserBindingName* InitializeIndexedBindings(
    SlotInfo& slotInfo, ParserBindingName* start, ParserBindingName* cursor,
    UnsignedInteger SlotInfo::*field, const ParserBindingNameVector& bindings,
    Step&&... step) {
  slotInfo.*field =
      AssertedCast<UnsignedInteger>(PointerRangeSize(start, cursor));

  ParserBindingName* newCursor =
      std::uninitialized_copy(bindings.begin(), bindings.end(), cursor);

  return InitializeIndexedBindings(slotInfo, start, newCursor,
                                   std::forward<Step>(step)...);
}

}  // namespace detail

// Initialize the trailing name bindings of |data|, then set |data->length| to
// the count of bindings added (which must equal |count|).
//
// First, |firstBindings| are added to the trailing names.  Then any
// "steps" present are performed first to last.  Each step is 1) a pointer to a
// member of |data| to be set to the current number of bindings added, and 2) a
// vector of |ParserBindingName|s to then copy into |data->trailingNames|.
// (Thus each |data| member field indicates where the corresponding vector's
//  names start.)
template <class Data, typename... Step>
static MOZ_ALWAYS_INLINE void InitializeBindingData(
    Data* data, uint32_t count, const ParserBindingNameVector& firstBindings,
    Step&&... step) {
  MOZ_ASSERT(data->length == 0, "data shouldn't be filled yet");

  ParserBindingName* start = GetScopeDataTrailingNamesPointer(data);
  ParserBindingName* cursor = std::uninitialized_copy(
      firstBindings.begin(), firstBindings.end(), start);

#ifdef DEBUG
  ParserBindingName* end =
#endif
      detail::InitializeIndexedBindings(data->slotInfo, start, cursor,
                                        std::forward<Step>(step)...);

  MOZ_ASSERT(PointerRangeSize(start, end) == count);
  data->length = count;
}

static Maybe<GlobalScope::ParserData*> NewGlobalScopeData(
    FrontendContext* fc, ParseContext::Scope& scope, LifoAlloc& alloc,
    ParseContext* pc) {
  ParserBindingNameVector vars(fc);
  ParserBindingNameVector lets(fc);
  ParserBindingNameVector consts(fc);

  bool allBindingsClosedOver = pc->sc()->allBindingsClosedOver();
  for (BindingIter bi = scope.bindings(pc); bi; bi++) {
    bool closedOver = allBindingsClosedOver || bi.closedOver();

    switch (bi.kind()) {
      case BindingKind::Var: {
        bool isTopLevelFunction =
            bi.declarationKind() == DeclarationKind::BodyLevelFunction;

        ParserBindingName binding(bi.name(), closedOver, isTopLevelFunction);
        if (!vars.append(binding)) {
          return Nothing();
        }
        break;
      }
      case BindingKind::Let: {
        ParserBindingName binding(bi.name(), closedOver);
        if (!lets.append(binding)) {
          return Nothing();
        }
        break;
      }
      case BindingKind::Const: {
        ParserBindingName binding(bi.name(), closedOver);
        if (!consts.append(binding)) {
          return Nothing();
        }
        break;
      }
      default:
        MOZ_CRASH("Bad global scope BindingKind");
    }
  }

  GlobalScope::ParserData* bindings = nullptr;
  uint32_t numBindings = vars.length() + lets.length() + consts.length();

  if (numBindings > 0) {
    bindings = NewEmptyBindingData<GlobalScope>(fc, alloc, numBindings);
    if (!bindings) {
      return Nothing();
    }

    // The ordering here is important. See comments in GlobalScope.
    InitializeBindingData(bindings, numBindings, vars,
                          &ParserGlobalScopeSlotInfo::letStart, lets,
                          &ParserGlobalScopeSlotInfo::constStart, consts);
  }

  return Some(bindings);
}

Maybe<GlobalScope::ParserData*> ParserBase::newGlobalScopeData(
    ParseContext::Scope& scope) {
  return NewGlobalScopeData(fc_, scope, stencilAlloc(), pc_);
}

static Maybe<ModuleScope::ParserData*> NewModuleScopeData(
    FrontendContext* fc, ParseContext::Scope& scope, LifoAlloc& alloc,
    ParseContext* pc) {
  ParserBindingNameVector imports(fc);
  ParserBindingNameVector vars(fc);
  ParserBindingNameVector lets(fc);
  ParserBindingNameVector consts(fc);
#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
  ParserBindingNameVector usings(fc);
#endif

  bool allBindingsClosedOver =
      pc->sc()->allBindingsClosedOver() || scope.tooBigToOptimize();

  for (BindingIter bi = scope.bindings(pc); bi; bi++) {
    // Imports are indirect bindings and must not be given known slots.
    ParserBindingName binding(bi.name(),
                              (allBindingsClosedOver || bi.closedOver()) &&
                                  bi.kind() != BindingKind::Import);
    switch (bi.kind()) {
      case BindingKind::Import:
        if (!imports.append(binding)) {
          return Nothing();
        }
        break;
      case BindingKind::Var:
        if (!vars.append(binding)) {
          return Nothing();
        }
        break;
      case BindingKind::Let:
        if (!lets.append(binding)) {
          return Nothing();
        }
        break;
      case BindingKind::Const:
        if (!consts.append(binding)) {
          return Nothing();
        }
        break;
#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
      case BindingKind::Using:
        if (!usings.append(binding)) {
          return Nothing();
        }
        break;
#endif
      default:
        MOZ_CRASH("Bad module scope BindingKind");
    }
  }

  ModuleScope::ParserData* bindings = nullptr;
  uint32_t numBindings = imports.length() + vars.length() + lets.length() +
                         consts.length()
#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
                         + usings.length()
#endif
      ;

  if (numBindings > 0) {
    bindings = NewEmptyBindingData<ModuleScope>(fc, alloc, numBindings);
    if (!bindings) {
      return Nothing();
    }

    // The ordering here is important. See comments in ModuleScope.
    InitializeBindingData(bindings, numBindings, imports,
                          &ParserModuleScopeSlotInfo::varStart, vars,
                          &ParserModuleScopeSlotInfo::letStart, lets,
                          &ParserModuleScopeSlotInfo::constStart, consts
#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
                          ,
                          &ParserModuleScopeSlotInfo::usingStart, usings
#endif
    );
  }

  return Some(bindings);
}

Maybe<ModuleScope::ParserData*> ParserBase::newModuleScopeData(
    ParseContext::Scope& scope) {
  return NewModuleScopeData(fc_, scope, stencilAlloc(), pc_);
}

static Maybe<EvalScope::ParserData*> NewEvalScopeData(
    FrontendContext* fc, ParseContext::Scope& scope, LifoAlloc& alloc,
    ParseContext* pc) {
  ParserBindingNameVector vars(fc);

  // Treat all bindings as closed over in non-strict eval.
  bool allBindingsClosedOver =
      !pc->sc()->strict() || pc->sc()->allBindingsClosedOver();
  for (BindingIter bi = scope.bindings(pc); bi; bi++) {
    // Eval scopes only contain 'var' bindings.
    MOZ_ASSERT(bi.kind() == BindingKind::Var);
    bool isTopLevelFunction =
        bi.declarationKind() == DeclarationKind::BodyLevelFunction;
    bool closedOver = allBindingsClosedOver || bi.closedOver();

    ParserBindingName binding(bi.name(), closedOver, isTopLevelFunction);
    if (!vars.append(binding)) {
      return Nothing();
    }
  }

  EvalScope::ParserData* bindings = nullptr;
  uint32_t numBindings = vars.length();

  if (numBindings > 0) {
    bindings = NewEmptyBindingData<EvalScope>(fc, alloc, numBindings);
    if (!bindings) {
      return Nothing();
    }

    InitializeBindingData(bindings, numBindings, vars);
  }

  return Some(bindings);
}

Maybe<EvalScope::ParserData*> ParserBase::newEvalScopeData(
    ParseContext::Scope& scope) {
  return NewEvalScopeData(fc_, scope, stencilAlloc(), pc_);
}

static Maybe<FunctionScope::ParserData*> NewFunctionScopeData(
    FrontendContext* fc, ParseContext::Scope& scope, bool hasParameterExprs,
    LifoAlloc& alloc, ParseContext* pc) {
  ParserBindingNameVector positionalFormals(fc);
  ParserBindingNameVector formals(fc);
  ParserBindingNameVector vars(fc);

  bool allBindingsClosedOver =
      pc->sc()->allBindingsClosedOver() || scope.tooBigToOptimize();
  bool argumentBindingsClosedOver =
      allBindingsClosedOver || pc->isGeneratorOrAsync();
  bool hasDuplicateParams = pc->functionBox()->hasDuplicateParameters;

  // Positional parameter names must be added in order of appearance as they are
  // referenced using argument slots.
  for (size_t i = 0; i < pc->positionalFormalParameterNames().length(); i++) {
    TaggedParserAtomIndex name = pc->positionalFormalParameterNames()[i];

    ParserBindingName bindName;
    if (name) {
      DeclaredNamePtr p = scope.lookupDeclaredName(name);

      // Do not consider any positional formal parameters closed over if
      // there are parameter defaults. It is the binding in the defaults
      // scope that is closed over instead.
      bool closedOver =
          argumentBindingsClosedOver || (p && p->value()->closedOver());

      // If the parameter name has duplicates, only the final parameter
      // name should be on the environment, as otherwise the environment
      // object would have multiple, same-named properties.
      if (hasDuplicateParams) {
        for (size_t j = pc->positionalFormalParameterNames().length() - 1;
             j > i; j--) {
          if (TaggedParserAtomIndex(pc->positionalFormalParameterNames()[j]) ==
              name) {
            closedOver = false;
            break;
          }
        }
      }

      bindName = ParserBindingName(name, closedOver);
    }

    if (!positionalFormals.append(bindName)) {
      return Nothing();
    }
  }

  for (BindingIter bi = scope.bindings(pc); bi; bi++) {
    ParserBindingName binding(bi.name(),
                              allBindingsClosedOver || bi.closedOver());
    switch (bi.kind()) {
      case BindingKind::FormalParameter:
        // Positional parameter names are already handled above.
        if (bi.declarationKind() == DeclarationKind::FormalParameter) {
          if (!formals.append(binding)) {
            return Nothing();
          }
        }
        break;
      case BindingKind::Var:
        // The only vars in the function scope when there are parameter
        // exprs, which induces a separate var environment, should be the
        // special bindings.
        MOZ_ASSERT_IF(hasParameterExprs,
                      FunctionScope::isSpecialName(bi.name()));
        if (!vars.append(binding)) {
          return Nothing();
        }
        break;
      case BindingKind::Let:
      case BindingKind::Const:
#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
      case BindingKind::Using:
#endif
        break;
      default:
        MOZ_CRASH("bad function scope BindingKind");
        break;
    }
  }

  FunctionScope::ParserData* bindings = nullptr;
  uint32_t numBindings =
      positionalFormals.length() + formals.length() + vars.length();

  if (numBindings > 0) {
    bindings = NewEmptyBindingData<FunctionScope>(fc, alloc, numBindings);
    if (!bindings) {
      return Nothing();
    }

    // The ordering here is important. See comments in FunctionScope.
    InitializeBindingData(
        bindings, numBindings, positionalFormals,
        &ParserFunctionScopeSlotInfo::nonPositionalFormalStart, formals,
        &ParserFunctionScopeSlotInfo::varStart, vars);
  }

  return Some(bindings);
}

// Compute if `NewFunctionScopeData` would return any binding list with any
// entry marked as closed-over. This is done without the need to allocate the
// binding list. If true, an EnvironmentObject will be needed at runtime.
bool FunctionScopeHasClosedOverBindings(ParseContext* pc) {
  bool allBindingsClosedOver = pc->sc()->allBindingsClosedOver() ||
                               pc->functionScope().tooBigToOptimize();

  for (BindingIter bi = pc->functionScope().bindings(pc); bi; bi++) {
    switch (bi.kind()) {
      case BindingKind::FormalParameter:
      case BindingKind::Var:
        if (allBindingsClosedOver || bi.closedOver()) {
          return true;
        }
        break;

      default:
        break;
    }
  }

  return false;
}

Maybe<FunctionScope::ParserData*> ParserBase::newFunctionScopeData(
    ParseContext::Scope& scope, bool hasParameterExprs) {
  return NewFunctionScopeData(fc_, scope, hasParameterExprs, stencilAlloc(),
                              pc_);
}

VarScope::ParserData* NewEmptyVarScopeData(FrontendContext* fc,
                                           LifoAlloc& alloc,
                                           uint32_t numBindings) {
  return NewEmptyBindingData<VarScope>(fc, alloc, numBindings);
}

static Maybe<VarScope::ParserData*> NewVarScopeData(FrontendContext* fc,
                                                    ParseContext::Scope& scope,
                                                    LifoAlloc& alloc,
                                                    ParseContext* pc) {
  ParserBindingNameVector vars(fc);

  bool allBindingsClosedOver =
      pc->sc()->allBindingsClosedOver() || scope.tooBigToOptimize();

  for (BindingIter bi = scope.bindings(pc); bi; bi++) {
    if (bi.kind() == BindingKind::Var) {
      ParserBindingName binding(bi.name(),
                                allBindingsClosedOver || bi.closedOver());
      if (!vars.append(binding)) {
        return Nothing();
      }
    } else {
      MOZ_ASSERT(
          bi.kind() == BindingKind::Let || bi.kind() == BindingKind::Const,
          "bad var scope BindingKind");
    }
  }

  VarScope::ParserData* bindings = nullptr;
  uint32_t numBindings = vars.length();

  if (numBindings > 0) {
    bindings = NewEmptyBindingData<VarScope>(fc, alloc, numBindings);
    if (!bindings) {
      return Nothing();
    }

    InitializeBindingData(bindings, numBindings, vars);
  }

  return Some(bindings);
}

// Compute if `NewVarScopeData` would return any binding list. This is done
// without allocate the binding list.
static bool VarScopeHasBindings(ParseContext* pc) {
  for (BindingIter bi = pc->varScope().bindings(pc); bi; bi++) {
    if (bi.kind() == BindingKind::Var) {
      return true;
    }
  }

  return false;
}

Maybe<VarScope::ParserData*> ParserBase::newVarScopeData(
    ParseContext::Scope& scope) {
  return NewVarScopeData(fc_, scope, stencilAlloc(), pc_);
}

static Maybe<LexicalScope::ParserData*> NewLexicalScopeData(
    FrontendContext* fc, ParseContext::Scope& scope, LifoAlloc& alloc,
    ParseContext* pc) {
  ParserBindingNameVector lets(fc);
  ParserBindingNameVector consts(fc);
#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
  ParserBindingNameVector usings(fc);
#endif

  bool allBindingsClosedOver =
      pc->sc()->allBindingsClosedOver() || scope.tooBigToOptimize();

  for (BindingIter bi = scope.bindings(pc); bi; bi++) {
    ParserBindingName binding(bi.name(),
                              allBindingsClosedOver || bi.closedOver());
    switch (bi.kind()) {
      case BindingKind::Let:
        if (!lets.append(binding)) {
          return Nothing();
        }
        break;
      case BindingKind::Const:
        if (!consts.append(binding)) {
          return Nothing();
        }
        break;
#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
      case BindingKind::Using:
        if (!usings.append(binding)) {
          return Nothing();
        }
        break;
#endif
      case BindingKind::Var:
      case BindingKind::FormalParameter:
        break;
      default:
        MOZ_CRASH("Bad lexical scope BindingKind");
        break;
    }
  }

  LexicalScope::ParserData* bindings = nullptr;
  uint32_t numBindings = lets.length() + consts.length()
#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
                         + usings.length()
#endif
      ;

  if (numBindings > 0) {
    bindings = NewEmptyBindingData<LexicalScope>(fc, alloc, numBindings);
    if (!bindings) {
      return Nothing();
    }

    // The ordering here is important. See comments in LexicalScope.
    InitializeBindingData(bindings, numBindings, lets,
                          &ParserLexicalScopeSlotInfo::constStart, consts
#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
                          ,
                          &ParserLexicalScopeSlotInfo::usingStart, usings
#endif
    );
  }

  return Some(bindings);
}

// Compute if `NewLexicalScopeData` would return any binding list with any entry
// marked as closed-over. This is done without the need to allocate the binding
// list. If true, an EnvironmentObject will be needed at runtime.
bool LexicalScopeHasClosedOverBindings(ParseContext* pc,
                                       ParseContext::Scope& scope) {
  bool allBindingsClosedOver =
      pc->sc()->allBindingsClosedOver() || scope.tooBigToOptimize();

  for (BindingIter bi = scope.bindings(pc); bi; bi++) {
    switch (bi.kind()) {
      case BindingKind::Let:
      case BindingKind::Const:
        if (allBindingsClosedOver || bi.closedOver()) {
          return true;
        }
        break;

      default:
        break;
    }
  }

  return false;
}

Maybe<LexicalScope::ParserData*> ParserBase::newLexicalScopeData(
    ParseContext::Scope& scope) {
  return NewLexicalScopeData(fc_, scope, stencilAlloc(), pc_);
}

static Maybe<ClassBodyScope::ParserData*> NewClassBodyScopeData(
    FrontendContext* fc, ParseContext::Scope& scope, LifoAlloc& alloc,
    ParseContext* pc) {
  ParserBindingNameVector privateBrand(fc);
  ParserBindingNameVector synthetics(fc);
  ParserBindingNameVector privateMethods(fc);

  bool allBindingsClosedOver =
      pc->sc()->allBindingsClosedOver() || scope.tooBigToOptimize();

  for (BindingIter bi = scope.bindings(pc); bi; bi++) {
    ParserBindingName binding(bi.name(),
                              allBindingsClosedOver || bi.closedOver());
    switch (bi.kind()) {
      case BindingKind::Synthetic:
        if (bi.name() ==
            TaggedParserAtomIndex::WellKnown::dot_privateBrand_()) {
          MOZ_ASSERT(privateBrand.empty());
          if (!privateBrand.append(binding)) {
            return Nothing();
          }
        } else {
          if (!synthetics.append(binding)) {
            return Nothing();
          }
        }
        break;

      case BindingKind::PrivateMethod:
        if (!privateMethods.append(binding)) {
          return Nothing();
        }
        break;

      default:
        MOZ_CRASH("bad class body scope BindingKind");
        break;
    }
  }

  // We should have zero or one private brands.
  MOZ_ASSERT(privateBrand.length() == 0 || privateBrand.length() == 1);

  ClassBodyScope::ParserData* bindings = nullptr;
  uint32_t numBindings =
      privateBrand.length() + synthetics.length() + privateMethods.length();

  if (numBindings > 0) {
    bindings = NewEmptyBindingData<ClassBodyScope>(fc, alloc, numBindings);
    if (!bindings) {
      return Nothing();
    }
    // To simplify initialization of the bindings, we concatenate the
    // synthetics+privateBrand vector such that the private brand is always the
    // first element, as ordering is important. See comments in ClassBodyScope.
    ParserBindingNameVector brandAndSynthetics(fc);
    if (!brandAndSynthetics.appendAll(privateBrand)) {
      return Nothing();
    }
    if (!brandAndSynthetics.appendAll(synthetics)) {
      return Nothing();
    }

    // The ordering here is important. See comments in ClassBodyScope.
    InitializeBindingData(bindings, numBindings, brandAndSynthetics,
                          &ParserClassBodyScopeSlotInfo::privateMethodStart,
                          privateMethods);
  }

  // `EmitterScope::lookupPrivate()` requires `.privateBrand` to be stored in a
  // predictable slot: the first slot available in the environment object,
  // `ClassBodyLexicalEnvironmentObject::privateBrandSlot()`. We assume that
  // if `.privateBrand` is first in the scope, it will be stored there.
  MOZ_ASSERT_IF(!privateBrand.empty(),
                GetScopeDataTrailingNames(bindings)[0].name() ==
                    TaggedParserAtomIndex::WellKnown::dot_privateBrand_());

  return Some(bindings);
}

Maybe<ClassBodyScope::ParserData*> ParserBase::newClassBodyScopeData(
    ParseContext::Scope& scope) {
  return NewClassBodyScopeData(fc_, scope, stencilAlloc(), pc_);
}

template <>
SyntaxParseHandler::LexicalScopeNodeResult
PerHandlerParser<SyntaxParseHandler>::finishLexicalScope(
    ParseContext::Scope& scope, Node body, ScopeKind kind) {
  if (!propagateFreeNamesAndMarkClosedOverBindings(scope)) {
    return errorResult();
  }

  return handler_.newLexicalScope(body);
}

template <>
FullParseHandler::LexicalScopeNodeResult
PerHandlerParser<FullParseHandler>::finishLexicalScope(
    ParseContext::Scope& scope, ParseNode* body, ScopeKind kind) {
  if (!propagateFreeNamesAndMarkClosedOverBindings(scope)) {
    return errorResult();
  }

  Maybe<LexicalScope::ParserData*> bindings = newLexicalScopeData(scope);
  if (!bindings) {
    return errorResult();
  }

  return handler_.newLexicalScope(*bindings, body, kind);
}

template <>
SyntaxParseHandler::ClassBodyScopeNodeResult
PerHandlerParser<SyntaxParseHandler>::finishClassBodyScope(
    ParseContext::Scope& scope, ListNodeType body) {
  if (!propagateFreeNamesAndMarkClosedOverBindings(scope)) {
    return errorResult();
  }

  return handler_.newClassBodyScope(body);
}

template <>
FullParseHandler::ClassBodyScopeNodeResult
PerHandlerParser<FullParseHandler>::finishClassBodyScope(
    ParseContext::Scope& scope, ListNode* body) {
  if (!propagateFreeNamesAndMarkClosedOverBindings(scope)) {
    return errorResult();
  }

  Maybe<ClassBodyScope::ParserData*> bindings = newClassBodyScopeData(scope);
  if (!bindings) {
    return errorResult();
  }

  return handler_.newClassBodyScope(*bindings, body);
}

template <class ParseHandler>
bool PerHandlerParser<ParseHandler>::checkForUndefinedPrivateFields(
    EvalSharedContext* evalSc) {
  if (!this->compilationState_.isInitialStencil()) {
    // We're delazifying -- so we already checked private names during first
    // parse.
    return true;
  }

  Vector<UnboundPrivateName, 8> unboundPrivateNames(fc_);
  if (!usedNames_.getUnboundPrivateNames(unboundPrivateNames)) {
    return false;
  }

  // No unbound names, let's get out of here!
  if (unboundPrivateNames.empty()) {
    return true;
  }

  // It is an early error if there's private name references unbound,
  // unless it's an eval, in which case we need to check the scope
  // chain.
  if (!evalSc) {
    // The unbound private names are sorted, so just grab the first one.
    UnboundPrivateName minimum = unboundPrivateNames[0];
    UniqueChars str = this->parserAtoms().toPrintableString(minimum.atom);
    if (!str) {
      ReportOutOfMemory(this->fc_);
      return false;
    }

    errorAt(minimum.position.begin, JSMSG_MISSING_PRIVATE_DECL, str.get());
    return false;
  }

  // It's important that the unbound private names are sorted, as we
  // want our errors to always be issued to the first textually.
  for (UnboundPrivateName unboundName : unboundPrivateNames) {
    // If the enclosingScope is non-syntactic, then we are in a
    // Debugger.Frame.prototype.eval call. In order to find the declared private
    // names, we must use the effective scope that was determined when creating
    // the scopeContext.
    if (!this->compilationState_.scopeContext
             .effectiveScopePrivateFieldCacheHas(unboundName.atom)) {
      UniqueChars str = this->parserAtoms().toPrintableString(unboundName.atom);
      if (!str) {
        ReportOutOfMemory(this->fc_);
        return false;
      }
      errorAt(unboundName.position.begin, JSMSG_MISSING_PRIVATE_DECL,
              str.get());
      return false;
    }
  }

  return true;
}

template <typename Unit>
FullParseHandler::LexicalScopeNodeResult
Parser<FullParseHandler, Unit>::evalBody(EvalSharedContext* evalsc) {
  SourceParseContext evalpc(this, evalsc, /* newDirectives = */ nullptr);
  if (!evalpc.init()) {
    return errorResult();
  }

  ParseContext::VarScope varScope(this);
  if (!varScope.init(pc_)) {
    return errorResult();
  }

  LexicalScopeNode* body;
  {
    // All evals have an implicit non-extensible lexical scope.
    ParseContext::Scope lexicalScope(this);
    if (!lexicalScope.init(pc_)) {
      return errorResult();
    }

    ListNode* list;
    MOZ_TRY_VAR(list, statementList(YieldIsName));

    if (!checkStatementsEOF()) {
      return errorResult();
    }

    // Private names not lexically defined must trigger a syntax error.
    if (!checkForUndefinedPrivateFields(evalsc)) {
      return errorResult();
    }

    MOZ_TRY_VAR(body, finishLexicalScope(lexicalScope, list));
  }

#ifdef DEBUG
  if (evalpc.superScopeNeedsHomeObject() &&
      !this->compilationState_.input.enclosingScope.isNull()) {
    // If superScopeNeedsHomeObject_ is set and we are an entry-point
    // ParseContext, then we must be emitting an eval script, and the
    // outer function must already be marked as needing a home object
    // since it contains an eval.
    MOZ_ASSERT(
        this->compilationState_.scopeContext.hasFunctionNeedsHomeObjectOnChain,
        "Eval must have found an enclosing function box scope that "
        "allows super.property");
  }
#endif

  if (!CheckParseTree(this->fc_, alloc_, body)) {
    return errorResult();
  }

  ParseNode* node = body;
  // Don't constant-fold inside "use asm" code, as this could create a parse
  // tree that doesn't type-check as asm.js.
  if (!pc_->useAsmOrInsideUseAsm()) {
    if (!FoldConstants(this->fc_, this->parserAtoms(), &node, &handler_)) {
      return errorResult();
    }
  }
  body = handler_.asLexicalScopeNode(node);

  if (!this->setSourceMapInfo()) {
    return errorResult();
  }

  if (pc_->sc()->strict()) {
    if (!propagateFreeNamesAndMarkClosedOverBindings(varScope)) {
      return errorResult();
    }
  } else {
    // For non-strict eval scripts, since all bindings are automatically
    // considered closed over, we don't need to call propagateFreeNames-
    // AndMarkClosedOverBindings. However, Annex B.3.3 functions still need to
    // be marked.
    if (!varScope.propagateAndMarkAnnexBFunctionBoxes(pc_, this)) {
      return errorResult();
    }
  }

  Maybe<EvalScope::ParserData*> bindings = newEvalScopeData(pc_->varScope());
  if (!bindings) {
    return errorResult();
  }
  evalsc->bindings = *bindings;

  return body;
}

template <typename Unit>
FullParseHandler::ListNodeResult Parser<FullParseHandler, Unit>::globalBody(
    GlobalSharedContext* globalsc) {
  SourceParseContext globalpc(this, globalsc, /* newDirectives = */ nullptr);
  if (!globalpc.init()) {
    return errorResult();
  }

  ParseContext::VarScope varScope(this);
  if (!varScope.init(pc_)) {
    return errorResult();
  }

  ListNode* body;
  MOZ_TRY_VAR(body, statementList(YieldIsName));

  if (!checkStatementsEOF()) {
    return errorResult();
  }

  if (!CheckParseTree(this->fc_, alloc_, body)) {
    return errorResult();
  }

  if (!checkForUndefinedPrivateFields()) {
    return errorResult();
  }

  ParseNode* node = body;
  // Don't constant-fold inside "use asm" code, as this could create a parse
  // tree that doesn't type-check as asm.js.
  if (!pc_->useAsmOrInsideUseAsm()) {
    if (!FoldConstants(this->fc_, this->parserAtoms(), &node, &handler_)) {
      return errorResult();
    }
  }
  body = &node->as<ListNode>();

  if (!this->setSourceMapInfo()) {
    return errorResult();
  }

  // For global scripts, whether bindings are closed over or not doesn't
  // matter, so no need to call propagateFreeNamesAndMarkClosedOver-
  // Bindings. However, Annex B.3.3 functions still need to be marked.
  if (!varScope.propagateAndMarkAnnexBFunctionBoxes(pc_, this)) {
    return errorResult();
  }

  Maybe<GlobalScope::ParserData*> bindings =
      newGlobalScopeData(pc_->varScope());
  if (!bindings) {
    return errorResult();
  }
  globalsc->bindings = *bindings;

  return body;
}

template <typename Unit>
FullParseHandler::ModuleNodeResult Parser<FullParseHandler, Unit>::moduleBody(
    ModuleSharedContext* modulesc) {
  MOZ_ASSERT(checkOptionsCalled_);

  this->compilationState_.moduleMetadata =
      fc_->getAllocator()->template new_<StencilModuleMetadata>();
  if (!this->compilationState_.moduleMetadata) {
    return errorResult();
  }

  SourceParseContext modulepc(this, modulesc, nullptr);
  if (!modulepc.init()) {
    return errorResult();
  }

  ParseContext::VarScope varScope(this);
  if (!varScope.init(pc_)) {
    return errorResult();
  }

  ModuleNodeType moduleNode;
  MOZ_TRY_VAR(moduleNode, handler_.newModule(pos()));

  AutoAwaitIsKeyword<FullParseHandler, Unit> awaitIsKeyword(
      this, AwaitIsModuleKeyword);
  ListNode* stmtList;
  MOZ_TRY_VAR(stmtList, statementList(YieldIsName));

  MOZ_ASSERT(stmtList->isKind(ParseNodeKind::StatementList));
  moduleNode->setBody(&stmtList->template as<ListNode>());

  if (pc_->isAsync()) {
    if (!noteUsedName(TaggedParserAtomIndex::WellKnown::dot_generator_())) {
      return errorResult();
    }

    if (!pc_->declareTopLevelDotGeneratorName()) {
      return errorResult();
    }
  }

  TokenKind tt;
  if (!tokenStream.getToken(&tt, TokenStream::SlashIsRegExp)) {
    return errorResult();
  }
  if (tt != TokenKind::Eof) {
    error(JSMSG_GARBAGE_AFTER_INPUT, "module", TokenKindToDesc(tt));
    return errorResult();
  }

  // Set the module to async if an await keyword was found at the top level.
  if (pc_->isAsync()) {
    pc_->sc()->asModuleContext()->builder.noteAsync(
        *this->compilationState_.moduleMetadata);
  }

  // Generate the Import/Export tables and store in CompilationState.
  if (!modulesc->builder.buildTables(*this->compilationState_.moduleMetadata)) {
    return errorResult();
  }

  // Check exported local bindings exist and mark them as closed over.
  StencilModuleMetadata& moduleMetadata =
      *this->compilationState_.moduleMetadata;
  for (auto entry : moduleMetadata.localExportEntries) {
    DeclaredNamePtr p = modulepc.varScope().lookupDeclaredName(entry.localName);
    if (!p) {
      UniqueChars str = this->parserAtoms().toPrintableString(entry.localName);
      if (!str) {
        ReportOutOfMemory(this->fc_);
        return errorResult();
      }

      errorNoOffset(JSMSG_MISSING_EXPORT, str.get());
      return errorResult();
    }

    p->value()->setClosedOver();
  }

  // Reserve an environment slot for a "*namespace*" psuedo-binding and mark as
  // closed-over. We do not know until module linking if this will be used.
  if (!noteDeclaredName(
          TaggedParserAtomIndex::WellKnown::star_namespace_star_(),
          DeclarationKind::Const, pos())) {
    return errorResult();
  }
  modulepc.varScope()
      .lookupDeclaredName(
          TaggedParserAtomIndex::WellKnown::star_namespace_star_())
      ->value()
      ->setClosedOver();

  if (options().deoptimizeModuleGlobalVars) {
    for (BindingIter bi = modulepc.varScope().bindings(pc_); bi; bi++) {
      bi.setClosedOver();
    }
  }

  if (!CheckParseTree(this->fc_, alloc_, stmtList)) {
    return errorResult();
  }

  ParseNode* node = stmtList;
  // Don't constant-fold inside "use asm" code, as this could create a parse
  // tree that doesn't type-check as asm.js.
  if (!pc_->useAsmOrInsideUseAsm()) {
    if (!FoldConstants(this->fc_, this->parserAtoms(), &node, &handler_)) {
      return errorResult();
    }
  }
  stmtList = &node->as<ListNode>();

  if (!this->setSourceMapInfo()) {
    return errorResult();
  }

  // Private names not lexically defined must trigger a syntax error.
  if (!checkForUndefinedPrivateFields()) {
    return errorResult();
  }

  if (!propagateFreeNamesAndMarkClosedOverBindings(modulepc.varScope())) {
    return errorResult();
  }

  Maybe<ModuleScope::ParserData*> bindings =
      newModuleScopeData(modulepc.varScope());
  if (!bindings) {
    return errorResult();
  }

  modulesc->bindings = *bindings;
  return moduleNode;
}

template <typename Unit>
SyntaxParseHandler::ModuleNodeResult
Parser<SyntaxParseHandler, Unit>::moduleBody(ModuleSharedContext* modulesc) {
  MOZ_ALWAYS_FALSE(abortIfSyntaxParser());
  return errorResult();
}

template <class ParseHandler>
typename ParseHandler::NameNodeResult
PerHandlerParser<ParseHandler>::newInternalDotName(TaggedParserAtomIndex name) {
  NameNodeType nameNode;
  MOZ_TRY_VAR(nameNode, newName(name));
  if (!noteUsedName(name)) {
    return errorResult();
  }
  return nameNode;
}

template <class ParseHandler>
typename ParseHandler::NameNodeResult
PerHandlerParser<ParseHandler>::newThisName() {
  return newInternalDotName(TaggedParserAtomIndex::WellKnown::dot_this_());
}

template <class ParseHandler>
typename ParseHandler::NameNodeResult
PerHandlerParser<ParseHandler>::newNewTargetName() {
  return newInternalDotName(TaggedParserAtomIndex::WellKnown::dot_newTarget_());
}

template <class ParseHandler>
typename ParseHandler::NameNodeResult
PerHandlerParser<ParseHandler>::newDotGeneratorName() {
  return newInternalDotName(TaggedParserAtomIndex::WellKnown::dot_generator_());
}

template <class ParseHandler>
bool PerHandlerParser<ParseHandler>::finishFunctionScopes(
    bool isStandaloneFunction) {
  FunctionBox* funbox = pc_->functionBox();

  if (funbox->hasParameterExprs) {
    if (!propagateFreeNamesAndMarkClosedOverBindings(pc_->functionScope())) {
      return false;
    }

    // Functions with parameter expressions utilize the FunctionScope for vars
    // generated by sloppy-direct-evals, as well as arguments (which are
    // lexicals bindings). If the function body has var bindings (or has a
    // sloppy-direct-eval that might), then an extra VarScope must be created
    // for them.
    if (VarScopeHasBindings(pc_) ||
        funbox->needsExtraBodyVarEnvironmentRegardlessOfBindings()) {
      funbox->setFunctionHasExtraBodyVarScope();
    }
  }

  // See: JSFunction::needsCallObject()
  if (FunctionScopeHasClosedOverBindings(pc_) ||
      funbox->needsCallObjectRegardlessOfBindings()) {
    funbox->setNeedsFunctionEnvironmentObjects();
  }

  if (funbox->isNamedLambda() && !isStandaloneFunction) {
    if (!propagateFreeNamesAndMarkClosedOverBindings(pc_->namedLambdaScope())) {
      return false;
    }

    // See: JSFunction::needsNamedLambdaEnvironment()
    if (LexicalScopeHasClosedOverBindings(pc_, pc_->namedLambdaScope())) {
      funbox->setNeedsFunctionEnvironmentObjects();
    }
  }

  return true;
}

template <>
bool PerHandlerParser<FullParseHandler>::finishFunction(
    bool isStandaloneFunction /* = false */) {
  if (!finishFunctionScopes(isStandaloneFunction)) {
    return false;
  }

  FunctionBox* funbox = pc_->functionBox();
  ScriptStencil& script = funbox->functionStencil();

  if (funbox->isInterpreted()) {
    // BCE will need to generate bytecode for this.
    funbox->emitBytecode = true;
    this->compilationState_.nonLazyFunctionCount++;
  }

  bool hasParameterExprs = funbox->hasParameterExprs;

  if (hasParameterExprs) {
    Maybe<VarScope::ParserData*> bindings = newVarScopeData(pc_->varScope());
    if (!bindings) {
      return false;
    }
    funbox->setExtraVarScopeBindings(*bindings);

    MOZ_ASSERT(bool(*bindings) == VarScopeHasBindings(pc_));
    MOZ_ASSERT_IF(!funbox->needsExtraBodyVarEnvironmentRegardlessOfBindings(),
                  bool(*bindings) == funbox->functionHasExtraBodyVarScope());
  }

  {
    Maybe<FunctionScope::ParserData*> bindings =
        newFunctionScopeData(pc_->functionScope(), hasParameterExprs);
    if (!bindings) {
      return false;
    }
    funbox->setFunctionScopeBindings(*bindings);
  }

  if (funbox->isNamedLambda() && !isStandaloneFunction) {
    Maybe<LexicalScope::ParserData*> bindings =
        newLexicalScopeData(pc_->namedLambdaScope());
    if (!bindings) {
      return false;
    }
    funbox->setNamedLambdaBindings(*bindings);
  }

  funbox->finishScriptFlags();
  funbox->copyFunctionFields(script);

  if (this->compilationState_.isInitialStencil()) {
    ScriptStencilExtra& scriptExtra = funbox->functionExtraStencil();
    funbox->copyFunctionExtraFields(scriptExtra);
    funbox->copyScriptExtraFields(scriptExtra);
  }

  return true;
}

template <>
bool PerHandlerParser<SyntaxParseHandler>::finishFunction(
    bool isStandaloneFunction /* = false */) {
  // The BaseScript for a lazily parsed function needs to know its set of
  // free variables and inner functions so that when it is fully parsed, we
  // can skip over any already syntax parsed inner functions and still
  // retain correct scope information.

  if (!finishFunctionScopes(isStandaloneFunction)) {
    return false;
  }

  FunctionBox* funbox = pc_->functionBox();
  ScriptStencil& script = funbox->functionStencil();

  funbox->finishScriptFlags();
  funbox->copyFunctionFields(script);

  ScriptStencilExtra& scriptExtra = funbox->functionExtraStencil();
  funbox->copyFunctionExtraFields(scriptExtra);
  funbox->copyScriptExtraFields(scriptExtra);

  // Elide nullptr sentinels from end of binding list. These are inserted for
  // each scope regardless of if any bindings are actually closed over.
  {
    AtomVector& closedOver = pc_->closedOverBindingsForLazy();
    while (!closedOver.empty() && !closedOver.back()) {
      closedOver.popBack();
    }
  }

  // Check if we will overflow the `ngcthings` field later.
  mozilla::CheckedUint32 ngcthings =
      mozilla::CheckedUint32(pc_->innerFunctionIndexesForLazy.length()) +
      mozilla::CheckedUint32(pc_->closedOverBindingsForLazy().length());
  if (!ngcthings.isValid()) {
    ReportAllocationOverflow(fc_);
    return false;
  }

  // If there are no script-things, we can return early without allocating.
  if (ngcthings.value() == 0) {
    MOZ_ASSERT(!script.hasGCThings());
    return true;
  }

  TaggedScriptThingIndex* cursor = nullptr;
  if (!this->compilationState_.allocateGCThingsUninitialized(
          fc_, funbox->index(), ngcthings.value(), &cursor)) {
    return false;
  }

  // Copy inner-function and closed-over-binding info for the stencil. The order
  // is important here. We emit functions first, followed by the bindings info.
  // The bindings list uses nullptr as delimiter to separates the bindings per
  // scope.
  //
  // See: FullParseHandler::nextLazyInnerFunction(),
  //      FullParseHandler::nextLazyClosedOverBinding()
  for (const ScriptIndex& index : pc_->innerFunctionIndexesForLazy) {
    void* raw = &(*cursor++);
    new (raw) TaggedScriptThingIndex(index);
  }
  for (auto binding : pc_->closedOverBindingsForLazy()) {
    void* raw = &(*cursor++);
    if (binding) {
      this->parserAtoms().markUsedByStencil(binding, ParserAtom::Atomize::Yes);
      new (raw) TaggedScriptThingIndex(binding);
    } else {
      new (raw) TaggedScriptThingIndex();
    }
  }

  return true;
}

static YieldHandling GetYieldHandling(GeneratorKind generatorKind) {
  if (generatorKind == GeneratorKind::NotGenerator) {
    return YieldIsName;
  }
  return YieldIsKeyword;
}

static AwaitHandling GetAwaitHandling(FunctionAsyncKind asyncKind) {
  if (asyncKind == FunctionAsyncKind::SyncFunction) {
    return AwaitIsName;
  }
  return AwaitIsKeyword;
}

static FunctionFlags InitialFunctionFlags(FunctionSyntaxKind kind,
                                          GeneratorKind generatorKind,
                                          FunctionAsyncKind asyncKind,
                                          bool isSelfHosting) {
  FunctionFlags flags = {};

  switch (kind) {
    case FunctionSyntaxKind::Expression:
      flags = (generatorKind == GeneratorKind::NotGenerator &&
                       asyncKind == FunctionAsyncKind::SyncFunction
                   ? FunctionFlags::INTERPRETED_LAMBDA
                   : FunctionFlags::INTERPRETED_LAMBDA_GENERATOR_OR_ASYNC);
      break;
    case FunctionSyntaxKind::Arrow:
      flags = FunctionFlags::INTERPRETED_LAMBDA_ARROW;
      break;
    case FunctionSyntaxKind::Method:
    case FunctionSyntaxKind::FieldInitializer:
    case FunctionSyntaxKind::StaticClassBlock:
      flags = FunctionFlags::INTERPRETED_METHOD;
      break;
    case FunctionSyntaxKind::ClassConstructor:
    case FunctionSyntaxKind::DerivedClassConstructor:
      flags = FunctionFlags::INTERPRETED_CLASS_CTOR;
      break;
    case FunctionSyntaxKind::Getter:
      flags = FunctionFlags::INTERPRETED_GETTER;
      break;
    case FunctionSyntaxKind::Setter:
      flags = FunctionFlags::INTERPRETED_SETTER;
      break;
    default:
      MOZ_ASSERT(kind == FunctionSyntaxKind::Statement);
      flags = (generatorKind == GeneratorKind::NotGenerator &&
                       asyncKind == FunctionAsyncKind::SyncFunction
                   ? FunctionFlags::INTERPRETED_NORMAL
                   : FunctionFlags::INTERPRETED_GENERATOR_OR_ASYNC);
  }

  if (isSelfHosting) {
    flags.setIsSelfHostedBuiltin();
  }

  return flags;
}

template <typename Unit>
FullParseHandler::FunctionNodeResult
Parser<FullParseHandler, Unit>::standaloneFunction(
    const Maybe<uint32_t>& parameterListEnd, FunctionSyntaxKind syntaxKind,
    GeneratorKind generatorKind, FunctionAsyncKind asyncKind,
    Directives inheritedDirectives, Directives* newDirectives) {
  MOZ_ASSERT(checkOptionsCalled_);
  // Skip prelude.
  TokenKind tt;
  if (!tokenStream.getToken(&tt, TokenStream::SlashIsRegExp)) {
    return errorResult();
  }
  if (asyncKind == FunctionAsyncKind::AsyncFunction) {
    MOZ_ASSERT(tt == TokenKind::Async);
    if (!tokenStream.getToken(&tt, TokenStream::SlashIsRegExp)) {
      return errorResult();
    }
  }
  MOZ_ASSERT(tt == TokenKind::Function);

  if (!tokenStream.getToken(&tt)) {
    return errorResult();
  }
  if (generatorKind == GeneratorKind::Generator) {
    MOZ_ASSERT(tt == TokenKind::Mul);
    if (!tokenStream.getToken(&tt)) {
      return errorResult();
    }
  }

  // Skip function name, if present.
  TaggedParserAtomIndex explicitName;
  if (TokenKindIsPossibleIdentifierName(tt)) {
    explicitName = anyChars.currentName();
  } else {
    anyChars.ungetToken();
  }

  FunctionNodeType funNode;
  MOZ_TRY_VAR(funNode, handler_.newFunction(syntaxKind, pos()));

  ParamsBodyNodeType argsbody;
  MOZ_TRY_VAR(argsbody, handler_.newParamsBody(pos()));
  funNode->setBody(argsbody);

  bool isSelfHosting = options().selfHostingMode;
  FunctionFlags flags =
      InitialFunctionFlags(syntaxKind, generatorKind, asyncKind, isSelfHosting);
  FunctionBox* funbox =
      newFunctionBox(funNode, explicitName, flags, /* toStringStart = */ 0,
                     inheritedDirectives, generatorKind, asyncKind);
  if (!funbox) {
    return errorResult();
  }

  // Function is not syntactically part of another script.
  MOZ_ASSERT(funbox->index() == CompilationStencil::TopLevelIndex);

  funbox->initStandalone(this->compilationState_.scopeContext, syntaxKind);

  SourceParseContext funpc(this, funbox, newDirectives);
  if (!funpc.init()) {
    return errorResult();
  }

  YieldHandling yieldHandling = GetYieldHandling(generatorKind);
  AwaitHandling awaitHandling = GetAwaitHandling(asyncKind);
  AutoAwaitIsKeyword<FullParseHandler, Unit> awaitIsKeyword(this,
                                                            awaitHandling);
  if (!functionFormalParametersAndBody(InAllowed, yieldHandling, &funNode,
                                       syntaxKind, parameterListEnd,
                                       /* isStandaloneFunction = */ true)) {
    return errorResult();
  }

  if (!tokenStream.getToken(&tt, TokenStream::SlashIsRegExp)) {
    return errorResult();
  }
  if (tt != TokenKind::Eof) {
    error(JSMSG_GARBAGE_AFTER_INPUT, "function body", TokenKindToDesc(tt));
    return errorResult();
  }

  if (!CheckParseTree(this->fc_, alloc_, funNode)) {
    return errorResult();
  }

  ParseNode* node = funNode;
  // Don't constant-fold inside "use asm" code, as this could create a parse
  // tree that doesn't type-check as asm.js.
  if (!pc_->useAsmOrInsideUseAsm()) {
    if (!FoldConstants(this->fc_, this->parserAtoms(), &node, &handler_)) {
      return errorResult();
    }
  }
  funNode = &node->as<FunctionNode>();

  if (!checkForUndefinedPrivateFields(nullptr)) {
    return errorResult();
  }

  if (!this->setSourceMapInfo()) {
    return errorResult();
  }

  return funNode;
}

template <class ParseHandler, typename Unit>
typename ParseHandler::LexicalScopeNodeResult
GeneralParser<ParseHandler, Unit>::functionBody(InHandling inHandling,
                                                YieldHandling yieldHandling,
                                                FunctionSyntaxKind kind,
                                                FunctionBodyType type) {
  MOZ_ASSERT(pc_->isFunctionBox());

#ifdef DEBUG
  uint32_t startYieldOffset = pc_->lastYieldOffset;
#endif

  Node body;
  if (type == StatementListBody) {
    bool inheritedStrict = pc_->sc()->strict();
    MOZ_TRY_VAR(body, statementList(yieldHandling));

    // When we transitioned from non-strict to strict mode, we need to
    // validate that all parameter names are valid strict mode names.
    if (!inheritedStrict && pc_->sc()->strict()) {
      MOZ_ASSERT(pc_->sc()->hasExplicitUseStrict(),
                 "strict mode should only change when a 'use strict' directive "
                 "is present");
      if (!hasValidSimpleStrictParameterNames()) {
        // Request that this function be reparsed as strict to report
        // the invalid parameter name at the correct source location.
        pc_->newDirectives->setStrict();
        return errorResult();
      }
    }
  } else {
    MOZ_ASSERT(type == ExpressionBody);

    // Async functions are implemented as generators, and generators are
    // assumed to be statement lists, to prepend initial `yield`.
    ListNodeType stmtList = null();
    if (pc_->isAsync()) {
      MOZ_TRY_VAR(stmtList, handler_.newStatementList(pos()));
    }

    Node kid;
    MOZ_TRY_VAR(kid,
                assignExpr(inHandling, yieldHandling, TripledotProhibited));

    MOZ_TRY_VAR(body, handler_.newExpressionBody(kid));

    if (pc_->isAsync()) {
      handler_.addStatementToList(stmtList, body);
      body = stmtList;
    }
  }

  MOZ_ASSERT_IF(!pc_->isGenerator() && !pc_->isAsync(),
                pc_->lastYieldOffset == startYieldOffset);
  MOZ_ASSERT_IF(pc_->isGenerator(), kind != FunctionSyntaxKind::Arrow);
  MOZ_ASSERT_IF(pc_->isGenerator(), type == StatementListBody);

  if (pc_->needsDotGeneratorName()) {
    MOZ_ASSERT_IF(!pc_->isAsync(), type == StatementListBody);
    if (!pc_->declareDotGeneratorName()) {
      return errorResult();
    }
    if (pc_->isGenerator()) {
      NameNodeType generator;
      MOZ_TRY_VAR(generator, newDotGeneratorName());
      if (!handler_.prependInitialYield(handler_.asListNode(body), generator)) {
        return errorResult();
      }
    }
  }

  if (pc_->numberOfArgumentsNames > 0 || kind == FunctionSyntaxKind::Arrow) {
    MOZ_ASSERT(pc_->isFunctionBox());
    pc_->sc()->setIneligibleForArgumentsLength();
  }

  // Declare the 'arguments', 'this', and 'new.target' bindings if necessary
  // before finishing up the scope so these special bindings get marked as
  // closed over if necessary. Arrow functions don't have these bindings.
  if (kind != FunctionSyntaxKind::Arrow) {
    bool canSkipLazyClosedOverBindings = handler_.reuseClosedOverBindings();
    if (!pc_->declareFunctionArgumentsObject(usedNames_,
                                             canSkipLazyClosedOverBindings)) {
      return errorResult();
    }
    if (!pc_->declareFunctionThis(usedNames_, canSkipLazyClosedOverBindings)) {
      return errorResult();
    }
    if (!pc_->declareNewTarget(usedNames_, canSkipLazyClosedOverBindings)) {
      return errorResult();
    }
  }

  return finishLexicalScope(pc_->varScope(), body, ScopeKind::FunctionLexical);
}

template <class ParseHandler, typename Unit>
bool GeneralParser<ParseHandler, Unit>::matchOrInsertSemicolon(
    Modifier modifier /* = TokenStream::SlashIsRegExp */) {
  TokenKind tt = TokenKind::Eof;
  if (!tokenStream.peekTokenSameLine(&tt, modifier)) {
    return false;
  }
  if (tt != TokenKind::Eof && tt != TokenKind::Eol && tt != TokenKind::Semi &&
      tt != TokenKind::RightCurly) {
    /*
     * When current token is `await` and it's outside of async function,
     * it's possibly intended to be an await expression.
     *
     *   await f();
     *        ^
     *        |
     *        tried to insert semicolon here
     *
     * Detect this situation and throw an understandable error.  Otherwise
     * we'd throw a confusing "unexpected token: (unexpected token)" error.
     */
    if (!pc_->isAsync() && anyChars.currentToken().type == TokenKind::Await) {
      error(JSMSG_AWAIT_OUTSIDE_ASYNC_OR_MODULE);
      return false;
    }
    if (!yieldExpressionsSupported() &&
        anyChars.currentToken().type == TokenKind::Yield) {
      error(JSMSG_YIELD_OUTSIDE_GENERATOR);
      return false;
    }

#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
    if (!this->pc_->isUsingSyntaxAllowed() &&
        anyChars.currentToken().type == TokenKind::Using) {
      error(JSMSG_USING_OUTSIDE_BLOCK_OR_MODULE);
      return false;
    }
#endif

    /* Advance the scanner for proper error location reporting. */
    tokenStream.consumeKnownToken(tt, modifier);
    error(JSMSG_UNEXPECTED_TOKEN_NO_EXPECT, TokenKindToDesc(tt));
    return false;
  }
  bool matched;
  return tokenStream.matchToken(&matched, TokenKind::Semi, modifier);
}

bool ParserBase::leaveInnerFunction(ParseContext* outerpc) {
  MOZ_ASSERT(pc_ != outerpc);

  MOZ_ASSERT_IF(outerpc->isFunctionBox(),
                outerpc->functionBox()->index() < pc_->functionBox()->index());

  // If the current function allows super.property but cannot have a home
  // object, i.e., it is an arrow function, we need to propagate the flag to
  // the outer ParseContext.
  if (pc_->superScopeNeedsHomeObject()) {
    if (!pc_->isArrowFunction()) {
      MOZ_ASSERT(pc_->functionBox()->needsHomeObject());
    } else {
      outerpc->setSuperScopeNeedsHomeObject();
    }
  }

  // Lazy functions inner to another lazy function need to be remembered by
  // the inner function so that if the outer function is eventually parsed
  // we do not need any further parsing or processing of the inner function.
  //
  // Append the inner function index here unconditionally; the vector is only
  // used if the Parser using outerpc is a syntax parsing. See
  // GeneralParser<SyntaxParseHandler>::finishFunction.
  if (!outerpc->innerFunctionIndexesForLazy.append(
          pc_->functionBox()->index())) {
    return false;
  }

  PropagateTransitiveParseFlags(pc_->functionBox(), outerpc->sc());

  return true;
}

TaggedParserAtomIndex ParserBase::prefixAccessorName(
    PropertyType propType, TaggedParserAtomIndex propAtom) {
  StringBuffer prefixed(fc_);
  if (propType == PropertyType::Setter) {
    if (!prefixed.append("set ")) {
      return TaggedParserAtomIndex::null();
    }
  } else {
    if (!prefixed.append("get ")) {
      return TaggedParserAtomIndex::null();
    }
  }
  if (!prefixed.append(this->parserAtoms(), propAtom)) {
    return TaggedParserAtomIndex::null();
  }
  return prefixed.finishParserAtom(this->parserAtoms(), fc_);
}

template <class ParseHandler, typename Unit>
void GeneralParser<ParseHandler, Unit>::setFunctionStartAtPosition(
    FunctionBox* funbox, TokenPos pos) const {
  uint32_t startLine;
  JS::LimitedColumnNumberOneOrigin startColumn;
  tokenStream.computeLineAndColumn(pos.begin, &startLine, &startColumn);

  // NOTE: `Debugger::CallData::findScripts` relies on sourceStart and
  //       lineno/column referring to the same location.
  funbox->setStart(pos.begin, startLine, startColumn);
}

template <class ParseHandler, typename Unit>
void GeneralParser<ParseHandler, Unit>::setFunctionStartAtCurrentToken(
    FunctionBox* funbox) const {
  setFunctionStartAtPosition(funbox, anyChars.currentToken().pos);
}

template <class ParseHandler, typename Unit>
bool GeneralParser<ParseHandler, Unit>::functionArguments(
    YieldHandling yieldHandling, FunctionSyntaxKind kind,
    FunctionNodeType funNode) {
  FunctionBox* funbox = pc_->functionBox();

  // Modifier for the following tokens.
  // TokenStream::SlashIsDiv for the following cases:
  //   async a => 1
  //         ^
  //
  //   (a) => 1
  //   ^
  //
  //   async (a) => 1
  //         ^
  //
  //   function f(a) {}
  //             ^
  //
  // TokenStream::SlashIsRegExp for the following case:
  //   a => 1
  //   ^
  Modifier firstTokenModifier =
      kind != FunctionSyntaxKind::Arrow || funbox->isAsync()
          ? TokenStream::SlashIsDiv
          : TokenStream::SlashIsRegExp;
  TokenKind tt;
  if (!tokenStream.getToken(&tt, firstTokenModifier)) {
    return false;
  }

  if (kind == FunctionSyntaxKind::Arrow && TokenKindIsPossibleIdentifier(tt)) {
    // Record the start of function source (for FunctionToString).
    setFunctionStartAtCurrentToken(funbox);

    ParamsBodyNodeType argsbody;
    MOZ_TRY_VAR_OR_RETURN(argsbody, handler_.newParamsBody(pos()), false);
    handler_.setFunctionFormalParametersAndBody(funNode, argsbody);

    TaggedParserAtomIndex name = bindingIdentifier(yieldHandling);
    if (!name) {
      return false;
    }

    constexpr bool disallowDuplicateParams = true;
    bool duplicatedParam = false;
    if (!notePositionalFormalParameter(funNode, name, pos().begin,
                                       disallowDuplicateParams,
                                       &duplicatedParam)) {
      return false;
    }
    MOZ_ASSERT(!duplicatedParam);
    MOZ_ASSERT(pc_->positionalFormalParameterNames().length() == 1);

    funbox->setLength(1);
    funbox->setArgCount(1);
    return true;
  }

  if (tt != TokenKind::LeftParen) {
    error(kind == FunctionSyntaxKind::Arrow ? JSMSG_BAD_ARROW_ARGS
                                            : JSMSG_PAREN_BEFORE_FORMAL);
    return false;
  }

  // Record the start of function source (for FunctionToString).
  setFunctionStartAtCurrentToken(funbox);

  ParamsBodyNodeType argsbody;
  MOZ_TRY_VAR_OR_RETURN(argsbody, handler_.newParamsBody(pos()), false);
  handler_.setFunctionFormalParametersAndBody(funNode, argsbody);

  bool matched;
  if (!tokenStream.matchToken(&matched, TokenKind::RightParen,
                              TokenStream::SlashIsRegExp)) {
    return false;
  }
  if (!matched) {
    bool hasRest = false;
    bool hasDefault = false;
    bool duplicatedParam = false;
    bool disallowDuplicateParams =
        kind == FunctionSyntaxKind::Arrow ||
        kind == FunctionSyntaxKind::Method ||
        kind == FunctionSyntaxKind::FieldInitializer ||
        kind == FunctionSyntaxKind::ClassConstructor;
    AtomVector& positionalFormals = pc_->positionalFormalParameterNames();

    if (kind == FunctionSyntaxKind::Getter) {
      error(JSMSG_ACCESSOR_WRONG_ARGS, "getter", "no", "s");
      return false;
    }

    while (true) {
      if (hasRest) {
        error(JSMSG_PARAMETER_AFTER_REST);
        return false;
      }

      TokenKind tt;
      if (!tokenStream.getToken(&tt, TokenStream::SlashIsRegExp)) {
        return false;
      }

      if (tt == TokenKind::TripleDot) {
        if (kind == FunctionSyntaxKind::Setter) {
          error(JSMSG_ACCESSOR_WRONG_ARGS, "setter", "one", "");
          return false;
        }

        disallowDuplicateParams = true;
        if (duplicatedParam) {
          // Has duplicated args before the rest parameter.
          error(JSMSG_BAD_DUP_ARGS);
          return false;
        }

        hasRest = true;
        funbox->setHasRest();

        if (!tokenStream.getToken(&tt)) {
          return false;
        }

        if (!TokenKindIsPossibleIdentifier(tt) &&
            tt != TokenKind::LeftBracket && tt != TokenKind::LeftCurly) {
          error(JSMSG_NO_REST_NAME);
          return false;
        }
      }

      switch (tt) {
        case TokenKind::LeftBracket:
        case TokenKind::LeftCurly: {
          disallowDuplicateParams = true;
          if (duplicatedParam) {
            // Has duplicated args before the destructuring parameter.
            error(JSMSG_BAD_DUP_ARGS);
            return false;
          }

          funbox->hasDestructuringArgs = true;

          Node destruct;
          MOZ_TRY_VAR_OR_RETURN(
              destruct,
              destructuringDeclarationWithoutYieldOrAwait(
                  DeclarationKind::FormalParameter, yieldHandling, tt),
              false);

          if (!noteDestructuredPositionalFormalParameter(funNode, destruct)) {
            return false;
          }

          break;
        }

        default: {
          if (!TokenKindIsPossibleIdentifier(tt)) {
            error(JSMSG_MISSING_FORMAL);
            return false;
          }

          TaggedParserAtomIndex name = bindingIdentifier(yieldHandling);
          if (!name) {
            return false;
          }

          if (!notePositionalFormalParameter(funNode, name, pos().begin,
                                             disallowDuplicateParams,
                                             &duplicatedParam)) {
            return false;
          }
          if (duplicatedParam) {
            funbox->hasDuplicateParameters = true;
          }

          break;
        }
      }

      if (positionalFormals.length() >= ARGNO_LIMIT) {
        error(JSMSG_TOO_MANY_FUN_ARGS);
        return false;
      }

      bool matched;
      if (!tokenStream.matchToken(&matched, TokenKind::Assign,
                                  TokenStream::SlashIsRegExp)) {
        return false;
      }
      if (matched) {
        if (hasRest) {
          error(JSMSG_REST_WITH_DEFAULT);
          return false;
        }
        disallowDuplicateParams = true;
        if (duplicatedParam) {
          error(JSMSG_BAD_DUP_ARGS);
          return false;
        }

        if (!hasDefault) {
          hasDefault = true;

          // The Function.length property is the number of formals
          // before the first default argument.
          funbox->setLength(positionalFormals.length() - 1);
        }
        funbox->hasParameterExprs = true;

        Node def_expr;
        MOZ_TRY_VAR_OR_RETURN(
            def_expr, assignExprWithoutYieldOrAwait(yieldHandling), false);
        if (!handler_.setLastFunctionFormalParameterDefault(funNode,
                                                            def_expr)) {
          return false;
        }
      }

      // Setter syntax uniquely requires exactly one argument.
      if (kind == FunctionSyntaxKind::Setter) {
        break;
      }

      if (!tokenStream.matchToken(&matched, TokenKind::Comma,
                                  TokenStream::SlashIsRegExp)) {
        return false;
      }
      if (!matched) {
        break;
      }

      if (!hasRest) {
        if (!tokenStream.peekToken(&tt, TokenStream::SlashIsRegExp)) {
          return false;
        }
        if (tt == TokenKind::RightParen) {
          break;
        }
      }
    }

    TokenKind tt;
    if (!tokenStream.getToken(&tt, TokenStream::SlashIsRegExp)) {
      return false;
    }
    if (tt != TokenKind::RightParen) {
      if (kind == FunctionSyntaxKind::Setter) {
        error(JSMSG_ACCESSOR_WRONG_ARGS, "setter", "one", "");
        return false;
      }

      error(JSMSG_PAREN_AFTER_FORMAL);
      return false;
    }

    if (!hasDefault) {
      funbox->setLength(positionalFormals.length() - hasRest);
    }

    funbox->setArgCount(positionalFormals.length());
  } else if (kind == FunctionSyntaxKind::Setter) {
    error(JSMSG_ACCESSOR_WRONG_ARGS, "setter", "one", "");
    return false;
  }

  return true;
}

template <typename Unit>
bool Parser<FullParseHandler, Unit>::skipLazyInnerFunction(
    FunctionNode* funNode, uint32_t toStringStart, bool tryAnnexB) {
  // When a lazily-parsed function is called, we only fully parse (and emit)
  // that function, not any of its nested children. The initial syntax-only
  // parse recorded the free variables of nested functions and their extents,
  // so we can skip over them after accounting for their free variables.

  MOZ_ASSERT(pc_->isOutermostOfCurrentCompile());
  handler_.nextLazyInnerFunction();
  const ScriptStencil& cachedData = handler_.cachedScriptData();
  const ScriptStencilExtra& cachedExtra = handler_.cachedScriptExtra();
  MOZ_ASSERT(toStringStart == cachedExtra.extent.toStringStart);

  FunctionBox* funbox = newFunctionBox(funNode, cachedData, cachedExtra);
  if (!funbox) {
    return false;
  }

  ScriptStencil& script = funbox->functionStencil();
  funbox->copyFunctionFields(script);

  // If the inner lazy function is class constructor, connect it to the class
  // statement/expression we are parsing.
  if (funbox->isClassConstructor()) {
    auto classStmt =
        pc_->template findInnermostStatement<ParseContext::ClassStatement>();
    MOZ_ASSERT(!classStmt->constructorBox);
    classStmt->constructorBox = funbox;
  }

  MOZ_ASSERT_IF(pc_->isFunctionBox(),
                pc_->functionBox()->index() < funbox->index());

  PropagateTransitiveParseFlags(funbox, pc_->sc());

  if (!tokenStream.advance(funbox->extent().sourceEnd)) {
    return false;
  }

  // Append possible Annex B function box only upon successfully parsing.
  if (tryAnnexB &&
      !pc_->innermostScope()->addPossibleAnnexBFunctionBox(pc_, funbox)) {
    return false;
  }

  return true;
}

template <typename Unit>
bool Parser<SyntaxParseHandler, Unit>::skipLazyInnerFunction(
    FunctionNodeType funNode, uint32_t toStringStart, bool tryAnnexB) {
  MOZ_CRASH("Cannot skip lazy inner functions when syntax parsing");
}

template <class ParseHandler, typename Unit>
bool GeneralParser<ParseHandler, Unit>::skipLazyInnerFunction(
    FunctionNodeType funNode, uint32_t toStringStart, bool tryAnnexB) {
  return asFinalParser()->skipLazyInnerFunction(funNode, toStringStart,
                                                tryAnnexB);
}

template <class ParseHandler, typename Unit>
bool GeneralParser<ParseHandler, Unit>::addExprAndGetNextTemplStrToken(
    YieldHandling yieldHandling, ListNodeType nodeList, TokenKind* ttp) {
  Node pn;
  MOZ_TRY_VAR_OR_RETURN(pn, expr(InAllowed, yieldHandling, TripledotProhibited),
                        false);
  handler_.addList(nodeList, pn);

  TokenKind tt;
  if (!tokenStream.getToken(&tt, TokenStream::SlashIsRegExp)) {
    return false;
  }
  if (tt != TokenKind::RightCurly) {
    error(JSMSG_TEMPLSTR_UNTERM_EXPR);
    return false;
  }

  return tokenStream.getTemplateToken(ttp);
}

template <class ParseHandler, typename Unit>
bool GeneralParser<ParseHandler, Unit>::taggedTemplate(
    YieldHandling yieldHandling, ListNodeType tagArgsList, TokenKind tt) {
  CallSiteNodeType callSiteObjNode;
  MOZ_TRY_VAR_OR_RETURN(callSiteObjNode,
                        handler_.newCallSiteObject(pos().begin), false);
  handler_.addList(tagArgsList, callSiteObjNode);

  pc_->sc()->setHasCallSiteObj();

  while (true) {
    if (!appendToCallSiteObj(callSiteObjNode)) {
      return false;
    }
    if (tt != TokenKind::TemplateHead) {
      break;
    }

    if (!addExprAndGetNextTemplStrToken(yieldHandling, tagArgsList, &tt)) {
      return false;
    }
  }
  handler_.setEndPosition(tagArgsList, callSiteObjNode);
  return true;
}

template <class ParseHandler, typename Unit>
typename ParseHandler::ListNodeResult
GeneralParser<ParseHandler, Unit>::templateLiteral(
    YieldHandling yieldHandling) {
  NameNodeType literal;
  MOZ_TRY_VAR(literal, noSubstitutionUntaggedTemplate());

  ListNodeType nodeList;
  MOZ_TRY_VAR(nodeList,
              handler_.newList(ParseNodeKind::TemplateStringListExpr, literal));

  TokenKind tt;
  do {
    if (!addExprAndGetNextTemplStrToken(yieldHandling, nodeList, &tt)) {
      return errorResult();
    }

    MOZ_TRY_VAR(literal, noSubstitutionUntaggedTemplate());

    handler_.addList(nodeList, literal);
  } while (tt == TokenKind::TemplateHead);
  return nodeList;
}

template <class ParseHandler, typename Unit>
typename ParseHandler::FunctionNodeResult
GeneralParser<ParseHandler, Unit>::functionDefinition(
    FunctionNodeType funNode, uint32_t toStringStart, InHandling inHandling,
    YieldHandling yieldHandling, TaggedParserAtomIndex funName,
    FunctionSyntaxKind kind, GeneratorKind generatorKind,
    FunctionAsyncKind asyncKind, bool tryAnnexB /* = false */) {
  MOZ_ASSERT_IF(kind == FunctionSyntaxKind::Statement, funName);

  // If we see any inner function, note it on our current context. The bytecode
  // emitter may eliminate the function later, but we use a conservative
  // definition for consistency between lazy and full parsing.
  pc_->sc()->setHasInnerFunctions();

  // When fully parsing a lazy script, we do not fully reparse its inner
  // functions, which are also lazy. Instead, their free variables and source
  // extents are recorded and may be skipped.
  if (handler_.reuseLazyInnerFunctions()) {
    if (!skipLazyInnerFunction(funNode, toStringStart, tryAnnexB)) {
      return errorResult();
    }

    return funNode;
  }

  bool isSelfHosting = options().selfHostingMode;
  FunctionFlags flags =
      InitialFunctionFlags(kind, generatorKind, asyncKind, isSelfHosting);

  // Self-hosted functions with special function names require extended slots
  // for various purposes.
  bool forceExtended =
      isSelfHosting && funName &&
      this->parserAtoms().isExtendedUnclonedSelfHostedFunctionName(funName);
  if (forceExtended) {
    flags.setIsExtended();
  }

  // Speculatively parse using the directives of the parent parsing context.
  // If a directive is encountered (e.g., "use strict") that changes how the
  // function should have been parsed, we backup and reparse with the new set
  // of directives.
  Directives directives(pc_);
  Directives newDirectives = directives;

  Position start(tokenStream);
  auto startObj = this->compilationState_.getPosition();

  // Parse the inner function. The following is a loop as we may attempt to
  // reparse a function due to failed syntax parsing and encountering new
  // "use foo" directives.
  while (true) {
    if (trySyntaxParseInnerFunction(&funNode, funName, flags, toStringStart,
                                    inHandling, yieldHandling, kind,
                                    generatorKind, asyncKind, tryAnnexB,
                                    directives, &newDirectives)) {
      break;
    }

    // Return on error.
    if (anyChars.hadError() || directives == newDirectives) {
      return errorResult();
    }

    // Assignment must be monotonic to prevent infinitely attempting to
    // reparse.
    MOZ_ASSERT_IF(directives.strict(), newDirectives.strict());
    MOZ_ASSERT_IF(directives.asmJS(), newDirectives.asmJS());
    directives = newDirectives;

    // Rewind to retry parsing with new directives applied.
    tokenStream.rewind(start);
    this->compilationState_.rewind(startObj);

    // functionFormalParametersAndBody may have already set body before failing.
    handler_.setFunctionFormalParametersAndBody(funNode, null());
  }

  return funNode;
}

template <typename Unit>
bool Parser<FullParseHandler, Unit>::advancePastSyntaxParsedFunction(
    SyntaxParser* syntaxParser) {
  MOZ_ASSERT(getSyntaxParser() == syntaxParser);

  // Advance this parser over tokens processed by the syntax parser.
  Position currentSyntaxPosition(syntaxParser->tokenStream);
  if (!tokenStream.fastForward(currentSyntaxPosition, syntaxParser->anyChars)) {
    return false;
  }

  anyChars.adoptState(syntaxParser->anyChars);
  tokenStream.adoptState(syntaxParser->tokenStream);
  return true;
}

template <typename Unit>
bool Parser<FullParseHandler, Unit>::trySyntaxParseInnerFunction(
    FunctionNode** funNode, TaggedParserAtomIndex explicitName,
    FunctionFlags flags, uint32_t toStringStart, InHandling inHandling,
    YieldHandling yieldHandling, FunctionSyntaxKind kind,
    GeneratorKind generatorKind, FunctionAsyncKind asyncKind, bool tryAnnexB,
    Directives inheritedDirectives, Directives* newDirectives) {
  // Try a syntax parse for this inner function.
  do {
    // If we're assuming this function is an IIFE, always perform a full
    // parse to avoid the overhead of a lazy syntax-only parse. Although
    // the prediction may be incorrect, IIFEs are common enough that it
    // pays off for lots of code.
    if ((*funNode)->isLikelyIIFE() &&
        generatorKind == GeneratorKind::NotGenerator &&
        asyncKind == FunctionAsyncKind::SyncFunction) {
      break;
    }

    SyntaxParser* syntaxParser = getSyntaxParser();
    if (!syntaxParser) {
      break;
    }

    UsedNameTracker::RewindToken token = usedNames_.getRewindToken();
    auto statePosition = this->compilationState_.getPosition();

    // Move the syntax parser to the current position in the stream.  In the
    // common case this seeks forward, but it'll also seek backward *at least*
    // when arrow functions appear inside arrow function argument defaults
    // (because we rewind to reparse arrow functions once we're certain they're
    // arrow functions):
    //
    //   var x = (y = z => 2) => q;
    //   //           ^ we first seek to here to syntax-parse this function
    //   //      ^ then we seek back to here to syntax-parse the outer function
    Position currentPosition(tokenStream);
    if (!syntaxParser->tokenStream.seekTo(currentPosition, anyChars)) {
      return false;
    }

    // Make a FunctionBox before we enter the syntax parser, because |pn|
    // still expects a FunctionBox to be attached to it during BCE, and
    // the syntax parser cannot attach one to it.
    FunctionBox* funbox =
        newFunctionBox(*funNode, explicitName, flags, toStringStart,
                       inheritedDirectives, generatorKind, asyncKind);
    if (!funbox) {
      return false;
    }
    funbox->initWithEnclosingParseContext(pc_, kind);

    auto syntaxNodeResult = syntaxParser->innerFunctionForFunctionBox(
        SyntaxParseHandler::Node::NodeGeneric, pc_, funbox, inHandling,
        yieldHandling, kind, newDirectives);
    if (syntaxNodeResult.isErr()) {
      if (syntaxParser->hadAbortedSyntaxParse()) {
        // Try again with a full parse. UsedNameTracker needs to be
        // rewound to just before we tried the syntax parse for
        // correctness.
        syntaxParser->clearAbortedSyntaxParse();
        usedNames_.rewind(token);
        this->compilationState_.rewind(statePosition);
        MOZ_ASSERT(!fc_->hadErrors());
        break;
      }
      return false;
    }

    if (!advancePastSyntaxParsedFunction(syntaxParser)) {
      return false;
    }

    // Update the end position of the parse node.
    (*funNode)->pn_pos.end = anyChars.currentToken().pos.end;

    // Append possible Annex B function box only upon successfully parsing.
    if (tryAnnexB) {
      if (!pc_->innermostScope()->addPossibleAnnexBFunctionBox(pc_, funbox)) {
        return false;
      }
    }

    return true;
  } while (false);

  // We failed to do a syntax parse above, so do the full parse.
  FunctionNodeType innerFunc;
  MOZ_TRY_VAR_OR_RETURN(
      innerFunc,
      innerFunction(*funNode, pc_, explicitName, flags, toStringStart,
                    inHandling, yieldHandling, kind, generatorKind, asyncKind,
                    tryAnnexB, inheritedDirectives, newDirectives),
      false);

  *funNode = innerFunc;
  return true;
}

template <typename Unit>
bool Parser<SyntaxParseHandler, Unit>::trySyntaxParseInnerFunction(
    FunctionNodeType* funNode, TaggedParserAtomIndex explicitName,
    FunctionFlags flags, uint32_t toStringStart, InHandling inHandling,
    YieldHandling yieldHandling, FunctionSyntaxKind kind,
    GeneratorKind generatorKind, FunctionAsyncKind asyncKind, bool tryAnnexB,
    Directives inheritedDirectives, Directives* newDirectives) {
  // This is already a syntax parser, so just parse the inner function.
  FunctionNodeType innerFunc;
  MOZ_TRY_VAR_OR_RETURN(
      innerFunc,
      innerFunction(*funNode, pc_, explicitName, flags, toStringStart,
                    inHandling, yieldHandling, kind, generatorKind, asyncKind,
                    tryAnnexB, inheritedDirectives, newDirectives),
      false);

  *funNode = innerFunc;
  return true;
}

template <class ParseHandler, typename Unit>
inline bool GeneralParser<ParseHandler, Unit>::trySyntaxParseInnerFunction(
    FunctionNodeType* funNode, TaggedParserAtomIndex explicitName,
    FunctionFlags flags, uint32_t toStringStart, InHandling inHandling,
    YieldHandling yieldHandling, FunctionSyntaxKind kind,
    GeneratorKind generatorKind, FunctionAsyncKind asyncKind, bool tryAnnexB,
    Directives inheritedDirectives, Directives* newDirectives) {
  return asFinalParser()->trySyntaxParseInnerFunction(
      funNode, explicitName, flags, toStringStart, inHandling, yieldHandling,
      kind, generatorKind, asyncKind, tryAnnexB, inheritedDirectives,
      newDirectives);
}

template <class ParseHandler, typename Unit>
typename ParseHandler::FunctionNodeResult
GeneralParser<ParseHandler, Unit>::innerFunctionForFunctionBox(
    FunctionNodeType funNode, ParseContext* outerpc, FunctionBox* funbox,
    InHandling inHandling, YieldHandling yieldHandling, FunctionSyntaxKind kind,
    Directives* newDirectives) {
  // Note that it is possible for outerpc != this->pc_, as we may be
  // attempting to syntax parse an inner function from an outer full
  // parser. In that case, outerpc is a SourceParseContext from the full parser
  // instead of the current top of the stack of the syntax parser.

  // Push a new ParseContext.
  SourceParseContext funpc(this, funbox, newDirectives);
  if (!funpc.init()) {
    return errorResult();
  }

  if (!functionFormalParametersAndBody(inHandling, yieldHandling, &funNode,
                                       kind)) {
    return errorResult();
  }

  if (!leaveInnerFunction(outerpc)) {
    return errorResult();
  }

  return funNode;
}

template <class ParseHandler, typename Unit>
typename ParseHandler::FunctionNodeResult
GeneralParser<ParseHandler, Unit>::innerFunction(
    FunctionNodeType funNode, ParseContext* outerpc,
    TaggedParserAtomIndex explicitName, FunctionFlags flags,
    uint32_t toStringStart, InHandling inHandling, YieldHandling yieldHandling,
    FunctionSyntaxKind kind, GeneratorKind generatorKind,
    FunctionAsyncKind asyncKind, bool tryAnnexB, Directives inheritedDirectives,
    Directives* newDirectives) {
  // Note that it is possible for outerpc != this->pc_, as we may be
  // attempting to syntax parse an inner function from an outer full
  // parser. In that case, outerpc is a SourceParseContext from the full parser
  // instead of the current top of the stack of the syntax parser.

  FunctionBox* funbox =
      newFunctionBox(funNode, explicitName, flags, toStringStart,
                     inheritedDirectives, generatorKind, asyncKind);
  if (!funbox) {
    return errorResult();
  }
  funbox->initWithEnclosingParseContext(outerpc, kind);

  FunctionNodeType innerFunc;
  MOZ_TRY_VAR(innerFunc,
              innerFunctionForFunctionBox(funNode, outerpc, funbox, inHandling,
                                          yieldHandling, kind, newDirectives));

  // Append possible Annex B function box only upon successfully parsing.
  if (tryAnnexB) {
    if (!pc_->innermostScope()->addPossibleAnnexBFunctionBox(pc_, funbox)) {
      return errorResult();
    }
  }

  return innerFunc;
}

template <class ParseHandler, typename Unit>
bool GeneralParser<ParseHandler, Unit>::appendToCallSiteObj(
    CallSiteNodeType callSiteObj) {
  Node cookedNode;
  MOZ_TRY_VAR_OR_RETURN(cookedNode, noSubstitutionTaggedTemplate(), false);

  auto atom = tokenStream.getRawTemplateStringAtom();
  if (!atom) {
    return false;
  }
  NameNodeType rawNode;
  MOZ_TRY_VAR_OR_RETURN(rawNode, handler_.newTemplateStringLiteral(atom, pos()),
                        false);

  handler_.addToCallSiteObject(callSiteObj, rawNode, cookedNode);
  return true;
}

template <typename Unit>
FullParseHandler::FunctionNodeResult
Parser<FullParseHandler, Unit>::standaloneLazyFunction(
    CompilationInput& input, uint32_t toStringStart, bool strict,
    GeneratorKind generatorKind, FunctionAsyncKind asyncKind) {
  MOZ_ASSERT(checkOptionsCalled_);

  FunctionSyntaxKind syntaxKind = input.functionSyntaxKind();
  FunctionNodeType funNode;
  MOZ_TRY_VAR(funNode, handler_.newFunction(syntaxKind, pos()));

  TaggedParserAtomIndex displayAtom =
      this->getCompilationState().previousParseCache.displayAtom();

  Directives directives(strict);
  FunctionBox* funbox =
      newFunctionBox(funNode, displayAtom, input.functionFlags(), toStringStart,
                     directives, generatorKind, asyncKind);
  if (!funbox) {
    return errorResult();
  }
  const ScriptStencilExtra& funExtra =
      this->getCompilationState().previousParseCache.funExtra();
  funbox->initFromLazyFunction(
      funExtra, this->getCompilationState().scopeContext, syntaxKind);
  if (funbox->useMemberInitializers()) {
    funbox->setMemberInitializers(funExtra.memberInitializers());
  }

  Directives newDirectives = directives;
  SourceParseContext funpc(this, funbox, &newDirectives);
  if (!funpc.init()) {
    return errorResult();
  }

  // Our tokenStream has no current token, so funNode's position is garbage.
  // Substitute the position of the first token in our source.  If the
  // function is a not-async arrow, use TokenStream::SlashIsRegExp to keep
  // verifyConsistentModifier from complaining (we will use
  // TokenStream::SlashIsRegExp in functionArguments).
  Modifier modifier = (input.functionFlags().isArrow() &&
                       asyncKind == FunctionAsyncKind::SyncFunction)
                          ? TokenStream::SlashIsRegExp
                          : TokenStream::SlashIsDiv;
  if (!tokenStream.peekTokenPos(&funNode->pn_pos, modifier)) {
    return errorResult();
  }

  YieldHandling yieldHandling = GetYieldHandling(generatorKind);

  if (funbox->isSyntheticFunction()) {
    // Currently default class constructors are the only synthetic function that
    // supports delazification.
    MOZ_ASSERT(funbox->isClassConstructor());
    MOZ_ASSERT(funbox->extent().toStringStart == funbox->extent().sourceStart);

    HasHeritage hasHeritage = funbox->isDerivedClassConstructor()
                                  ? HasHeritage::Yes
                                  : HasHeritage::No;
    TokenPos synthesizedBodyPos(funbox->extent().toStringStart,
                                funbox->extent().toStringEnd);

    // Reset pos() to the `class` keyword for predictable results.
    tokenStream.consumeKnownToken(TokenKind::Class);

    if (!this->synthesizeConstructorBody(synthesizedBodyPos, hasHeritage,
                                         funNode, funbox)) {
      return errorResult();
    }
  } else {
    if (!functionFormalParametersAndBody(InAllowed, yieldHandling, &funNode,
                                         syntaxKind)) {
      MOZ_ASSERT(directives == newDirectives);
      return errorResult();
    }
  }

  if (!CheckParseTree(this->fc_, alloc_, funNode)) {
    return errorResult();
  }

  ParseNode* node = funNode;
  // Don't constant-fold inside "use asm" code, as this could create a parse
  // tree that doesn't type-check as asm.js.
  if (!pc_->useAsmOrInsideUseAsm()) {
    if (!FoldConstants(this->fc_, this->parserAtoms(), &node, &handler_)) {
      return errorResult();
    }
  }
  funNode = &node->as<FunctionNode>();

  return funNode;
}

void ParserBase::setFunctionEndFromCurrentToken(FunctionBox* funbox) const {
  if (compilationState_.isInitialStencil()) {
    MOZ_ASSERT(anyChars.currentToken().type != TokenKind::Eof);
    MOZ_ASSERT(anyChars.currentToken().type < TokenKind::Limit);
    funbox->setEnd(anyChars.currentToken().pos.end);
  } else {
    // If we're delazifying an arrow function with expression body and
    // the expression is also a function, we arrive here immediately after
    // skipping the function by Parser::skipLazyInnerFunction.
    //
    //   a => b => c
    //              ^
    //              |
    //              we're here
    //
    // In that case, the current token's type field is either Limit or
    // poisoned.
    // We shouldn't read the value if it's poisoned.
    // See TokenStreamSpecific<Unit, AnyCharsAccess>::advance and
    // mfbt/MemoryChecking.h for more details.
    //
    // Also, in delazification, the FunctionBox should already have the
    // correct extent, and we shouldn't overwrite it here.
    // See ScriptStencil variant of PerHandlerParser::newFunctionBox.
#if !defined(MOZ_ASAN) && !defined(MOZ_MSAN) && !defined(MOZ_VALGRIND)
    MOZ_ASSERT(anyChars.currentToken().type != TokenKind::Eof);
#endif
    MOZ_ASSERT(funbox->extent().sourceEnd == anyChars.currentToken().pos.end);
  }
}

template <class ParseHandler, typename Unit>
bool GeneralParser<ParseHandler, Unit>::functionFormalParametersAndBody(
    InHandling inHandling, YieldHandling yieldHandling,
    FunctionNodeType* funNode, FunctionSyntaxKind kind,
    const Maybe<uint32_t>& parameterListEnd /* = Nothing() */,
    bool isStandaloneFunction /* = false */) {
  // Given a properly initialized parse context, try to parse an actual
  // function without concern for conversion to strict mode, use of lazy
  // parsing and such.

  FunctionBox* funbox = pc_->functionBox();

  if (kind == FunctionSyntaxKind::ClassConstructor ||
      kind == FunctionSyntaxKind::DerivedClassConstructor) {
    if (!noteUsedName(TaggedParserAtomIndex::WellKnown::dot_initializers_())) {
      return false;
    }
#ifdef ENABLE_DECORATORS
    if (!noteUsedName(TaggedParserAtomIndex::WellKnown::
                          dot_instanceExtraInitializers_())) {
      return false;
    }
#endif
  }

  // See below for an explanation why arrow function parameters and arrow
  // function bodies are parsed with different yield/await settings.
  {
    AwaitHandling awaitHandling =
        kind == FunctionSyntaxKind::StaticClassBlock ? AwaitIsDisallowed
        : (funbox->isAsync() ||
           (kind == FunctionSyntaxKind::Arrow && awaitIsKeyword()))
            ? AwaitIsKeyword
            : AwaitIsName;
    AutoAwaitIsKeyword<ParseHandler, Unit> awaitIsKeyword(this, awaitHandling);
    AutoInParametersOfAsyncFunction<ParseHandler, Unit> inParameters(
        this, funbox->isAsync());
    if (!functionArguments(yieldHandling, kind, *funNode)) {
      return false;
    }
  }

  Maybe<ParseContext::VarScope> varScope;
  if (funbox->hasParameterExprs) {
    varScope.emplace(this);
    if (!varScope->init(pc_)) {
      return false;
    }
  } else {
    pc_->functionScope().useAsVarScope(pc_);
  }

  if (kind == FunctionSyntaxKind::Arrow) {
    TokenKind tt;
    if (!tokenStream.peekTokenSameLine(&tt)) {
      return false;
    }

    if (tt == TokenKind::Eol) {
      error(JSMSG_UNEXPECTED_TOKEN,
            "'=>' on the same line after an argument list",
            TokenKindToDesc(tt));
      return false;
    }
    if (tt != TokenKind::Arrow) {
      error(JSMSG_BAD_ARROW_ARGS);
      return false;
    }
    tokenStream.consumeKnownToken(TokenKind::Arrow);
  }

  // When parsing something for new Function() we have to make sure to
  // only treat a certain part of the source as a parameter list.
  if (parameterListEnd.isSome() && parameterListEnd.value() != pos().begin) {
    error(JSMSG_UNEXPECTED_PARAMLIST_END);
    return false;
  }

  // Parse the function body.
  FunctionBodyType bodyType = StatementListBody;
  TokenKind tt;
  if (!tokenStream.getToken(&tt, TokenStream::SlashIsRegExp)) {
    return false;
  }
  uint32_t openedPos = 0;
  if (tt != TokenKind::LeftCurly) {
    if (kind != FunctionSyntaxKind::Arrow) {
      error(JSMSG_CURLY_BEFORE_BODY);
      return false;
    }

    anyChars.ungetToken();
    bodyType = ExpressionBody;
    funbox->setHasExprBody();
  } else {
    openedPos = pos().begin;
  }

  // Arrow function parameters inherit yieldHandling from the enclosing
  // context, but the arrow body doesn't. E.g. in |(a = yield) => yield|,
  // |yield| in the parameters is either a name or keyword, depending on
  // whether the arrow function is enclosed in a generator function or not.
  // Whereas the |yield| in the function body is always parsed as a name.
  // The same goes when parsing |await| in arrow functions.
  YieldHandling bodyYieldHandling = GetYieldHandling(pc_->generatorKind());
  AwaitHandling bodyAwaitHandling = GetAwaitHandling(pc_->asyncKind());
  bool inheritedStrict = pc_->sc()->strict();
  LexicalScopeNodeType body;
  {
    AutoAwaitIsKeyword<ParseHandler, Unit> awaitIsKeyword(this,
                                                          bodyAwaitHandling);
    AutoInParametersOfAsyncFunction<ParseHandler, Unit> inParameters(this,
                                                                     false);
    MOZ_TRY_VAR_OR_RETURN(
        body, functionBody(inHandling, bodyYieldHandling, kind, bodyType),
        false);
  }

  // Revalidate the function name when we transitioned to strict mode.
  if ((kind == FunctionSyntaxKind::Statement ||
       kind == FunctionSyntaxKind::Expression) &&
      funbox->explicitName() && !inheritedStrict && pc_->sc()->strict()) {
    MOZ_ASSERT(pc_->sc()->hasExplicitUseStrict(),
               "strict mode should only change when a 'use strict' directive "
               "is present");

    auto propertyName = funbox->explicitName();
    YieldHandling nameYieldHandling;
    if (kind == FunctionSyntaxKind::Expression) {
      // Named lambda has binding inside it.
      nameYieldHandling = bodyYieldHandling;
    } else {
      // Otherwise YieldHandling cannot be checked at this point
      // because of different context.
      // It should already be checked before this point.
      nameYieldHandling = YieldIsName;
    }

    // We already use the correct await-handling at this point, therefore
    // we don't need call AutoAwaitIsKeyword here.

    uint32_t nameOffset = handler_.getFunctionNameOffset(*funNode, anyChars);
    if (!checkBindingIdentifier(propertyName, nameOffset, nameYieldHandling)) {
      return false;
    }
  }

  if (bodyType == StatementListBody) {
    // Cannot use mustMatchToken here because of internal compiler error on
    // gcc 6.4.0, with linux 64 SM hazard build.
    TokenKind actual;
    if (!tokenStream.getToken(&actual, TokenStream::SlashIsRegExp)) {
      return false;
    }
    if (actual != TokenKind::RightCurly) {
      reportMissingClosing(JSMSG_CURLY_AFTER_BODY, JSMSG_CURLY_OPENED,
                           openedPos);
      return false;
    }

    setFunctionEndFromCurrentToken(funbox);
  } else {
    MOZ_ASSERT(kind == FunctionSyntaxKind::Arrow);

    if (anyChars.hadError()) {
      return false;
    }

    setFunctionEndFromCurrentToken(funbox);

    if (kind == FunctionSyntaxKind::Statement) {
      if (!matchOrInsertSemicolon()) {
        return false;
      }
    }
  }

  if (IsMethodDefinitionKind(kind) && pc_->superScopeNeedsHomeObject()) {
    funbox->setNeedsHomeObject();
  }

  if (!finishFunction(isStandaloneFunction)) {
    return false;
  }

  handler_.setEndPosition(body, pos().begin);
  handler_.setEndPosition(*funNode, pos().end);
  handler_.setFunctionBody(*funNode, body);

  return true;
}

template <class ParseHandler, typename Unit>
typename ParseHandler::FunctionNodeResult
GeneralParser<ParseHandler, Unit>::functionStmt(uint32_t toStringStart,
                                                YieldHandling yieldHandling,
                                                DefaultHandling defaultHandling,
                                                FunctionAsyncKind asyncKind) {
  MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::Function));

  // In sloppy mode, Annex B.3.2 allows labelled function declarations.
  // Otherwise it's a parse error.
  ParseContext::Statement* declaredInStmt = pc_->innermostStatement();
  if (declaredInStmt && declaredInStmt->kind() == StatementKind::Label) {
    MOZ_ASSERT(!pc_->sc()->strict(),
               "labeled functions shouldn't be parsed in strict mode");

    // Find the innermost non-label statement.  Report an error if it's
    // unbraced: functions can't appear in it.  Otherwise the statement
    // (or its absence) determines the scope the function's bound in.
    while (declaredInStmt && declaredInStmt->kind() == StatementKind::Label) {
      declaredInStmt = declaredInStmt->enclosing();
    }

    if (declaredInStmt && !StatementKindIsBraced(declaredInStmt->kind())) {
      error(JSMSG_SLOPPY_FUNCTION_LABEL);
      return errorResult();
    }
  }

  TokenKind tt;
  if (!tokenStream.getToken(&tt)) {
    return errorResult();
  }

  GeneratorKind generatorKind = GeneratorKind::NotGenerator;
  if (tt == TokenKind::Mul) {
    generatorKind = GeneratorKind::Generator;
    if (!tokenStream.getToken(&tt)) {
      return errorResult();
    }
  }

  TaggedParserAtomIndex name;
  if (TokenKindIsPossibleIdentifier(tt)) {
    name = bindingIdentifier(yieldHandling);
    if (!name) {
      return errorResult();
    }
  } else if (defaultHandling == AllowDefaultName) {
    name = TaggedParserAtomIndex::WellKnown::default_();
    anyChars.ungetToken();
  } else {
    /* Unnamed function expressions are forbidden in statement context. */
    error(JSMSG_UNNAMED_FUNCTION_STMT);
    return errorResult();
  }

  // Note the declared name and check for early errors.
  DeclarationKind kind;
  if (declaredInStmt) {
    MOZ_ASSERT(declaredInStmt->kind() != StatementKind::Label);
    MOZ_ASSERT(StatementKindIsBraced(declaredInStmt->kind()));

    kind =
        (!pc_->sc()->strict() && generatorKind == GeneratorKind::NotGenerator &&
         asyncKind == FunctionAsyncKind::SyncFunction)
            ? DeclarationKind::SloppyLexicalFunction
            : DeclarationKind::LexicalFunction;
  } else {
    kind = pc_->atModuleLevel() ? DeclarationKind::ModuleBodyLevelFunction
                                : DeclarationKind::BodyLevelFunction;
  }

  if (!noteDeclaredName(name, kind, pos())) {
    return errorResult();
  }

  FunctionSyntaxKind syntaxKind = FunctionSyntaxKind::Statement;
  FunctionNodeType funNode;
  MOZ_TRY_VAR(funNode, handler_.newFunction(syntaxKind, pos()));

  // Under sloppy mode, try Annex B.3.3 semantics. If making an additional
  // 'var' binding of the same name does not throw an early error, do so.
  // This 'var' binding would be assigned the function object when its
  // declaration is reached, not at the start of the block.
  //
  // This semantics is implemented upon Scope exit in
  // Scope::propagateAndMarkAnnexBFunctionBoxes.
  bool tryAnnexB = kind == DeclarationKind::SloppyLexicalFunction;

  YieldHandling newYieldHandling = GetYieldHandling(generatorKind);
  return functionDefinition(funNode, toStringStart, InAllowed, newYieldHandling,
                            name, syntaxKind, generatorKind, asyncKind,
                            tryAnnexB);
}

template <class ParseHandler, typename Unit>
typename ParseHandler::FunctionNodeResult
GeneralParser<ParseHandler, Unit>::functionExpr(uint32_t toStringStart,
                                                InvokedPrediction invoked,
                                                FunctionAsyncKind asyncKind) {
  MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::Function));

  AutoAwaitIsKeyword<ParseHandler, Unit> awaitIsKeyword(
      this, GetAwaitHandling(asyncKind));
  GeneratorKind generatorKind = GeneratorKind::NotGenerator;
  TokenKind tt;
  if (!tokenStream.getToken(&tt)) {
    return errorResult();
  }

  if (tt == TokenKind::Mul) {
    generatorKind = GeneratorKind::Generator;
    if (!tokenStream.getToken(&tt)) {
      return errorResult();
    }
  }

  YieldHandling yieldHandling = GetYieldHandling(generatorKind);

  TaggedParserAtomIndex name;
  if (TokenKindIsPossibleIdentifier(tt)) {
    name = bindingIdentifier(yieldHandling);
    if (!name) {
      return errorResult();
    }
  } else {
    anyChars.ungetToken();
  }

  FunctionSyntaxKind syntaxKind = FunctionSyntaxKind::Expression;
  FunctionNodeType funNode;
  MOZ_TRY_VAR(funNode, handler_.newFunction(syntaxKind, pos()));

  if (invoked) {
    funNode = handler_.setLikelyIIFE(funNode);
  }

  return functionDefinition(funNode, toStringStart, InAllowed, yieldHandling,
                            name, syntaxKind, generatorKind, asyncKind);
}

/*
 * Return true if this node, known to be an unparenthesized string literal
 * that never contain escape sequences, could be the string of a directive in a
 * Directive Prologue. Directive strings never contain escape sequences or line
 * continuations.
 */
static inline bool IsUseStrictDirective(const TokenPos& pos,
                                        TaggedParserAtomIndex atom) {
  // the length of "use strict", including quotation.
  static constexpr size_t useStrictLength = 12;
  return atom == TaggedParserAtomIndex::WellKnown::use_strict_() &&
         pos.begin + useStrictLength == pos.end;
}
static inline bool IsUseAsmDirective(const TokenPos& pos,
                                     TaggedParserAtomIndex atom) {
  // the length of "use asm", including quotation.
  static constexpr size_t useAsmLength = 9;
  return atom == TaggedParserAtomIndex::WellKnown::use_asm_() &&
         pos.begin + useAsmLength == pos.end;
}

template <typename Unit>
bool Parser<SyntaxParseHandler, Unit>::asmJS(ListNodeType list) {
  // While asm.js could technically be validated and compiled during syntax
  // parsing, we have no guarantee that some later JS wouldn't abort the
  // syntax parse and cause us to re-parse (and re-compile) the asm.js module.
  // For simplicity, unconditionally abort the syntax parse when "use asm" is
  // encountered so that asm.js is always validated/compiled exactly once
  // during a full parse.
  MOZ_ALWAYS_FALSE(abortIfSyntaxParser());
  return false;
}

template <typename Unit>
bool Parser<FullParseHandler, Unit>::asmJS(ListNodeType list) {
  // Disable syntax parsing in anything nested inside the asm.js module.
  disableSyntaxParser();

  // We should be encountering the "use asm" directive for the first time; if
  // the directive is already, we must have failed asm.js validation and we're
  // reparsing. In that case, don't try to validate again. A non-null
  // newDirectives means we're not in a normal function.
  if (!pc_->newDirectives || pc_->newDirectives->asmJS()) {
    return true;
  }

  // If there is no ScriptSource, then we are doing a non-compiling parse and
  // so we shouldn't (and can't, without a ScriptSource) compile.
  if (ss == nullptr) {
    return true;
  }

  pc_->functionBox()->useAsm = true;

  // Attempt to validate and compile this asm.js module. On success, the
  // tokenStream has been advanced to the closing }. On failure, the
  // tokenStream is in an indeterminate state and we must reparse the
  // function from the beginning. Reparsing is triggered by marking that a
  // new directive has been encountered and returning 'false'.
  bool validated;
  if (!CompileAsmJS(this->fc_, this->parserAtoms(), *this, list, &validated)) {
    return false;
  }
  if (!validated) {
    pc_->newDirectives->setAsmJS();
    return false;
  }

  return true;
}

template <class ParseHandler, typename Unit>
inline bool GeneralParser<ParseHandler, Unit>::asmJS(ListNodeType list) {
  return asFinalParser()->asmJS(list);
}

/*
 * Recognize Directive Prologue members and directives. Assuming |pn| is a
 * candidate for membership in a directive prologue, recognize directives and
 * set |pc_|'s flags accordingly. If |pn| is indeed part of a prologue, set its
 * |prologue| flag.
 *
 * Note that the following is a strict mode function:
 *
 * function foo() {
 *   "blah" // inserted semi colon
 *        "blurgh"
 *   "use\x20loose"
 *   "use strict"
 * }
 *
 * That is, even though "use\x20loose" can never be a directive, now or in the
 * future (because of the hex escape), the Directive Prologue extends through it
 * to the "use strict" statement, which is indeed a directive.
 */
template <class ParseHandler, typename Unit>
bool GeneralParser<ParseHandler, Unit>::maybeParseDirective(
    ListNodeType list, Node possibleDirective, bool* cont) {
  TokenPos directivePos;
  TaggedParserAtomIndex directive =
      handler_.isStringExprStatement(possibleDirective, &directivePos);

  *cont = !!directive;
  if (!*cont) {
    return true;
  }

  if (IsUseStrictDirective(directivePos, directive)) {
    // Functions with non-simple parameter lists (destructuring,
    // default or rest parameters) must not contain a "use strict"
    // directive.
    if (pc_->isFunctionBox()) {
      FunctionBox* funbox = pc_->functionBox();
      if (!funbox->hasSimpleParameterList()) {
        const char* parameterKind = funbox->hasDestructuringArgs
                                        ? "destructuring"
                                    : funbox->hasParameterExprs ? "default"
                                                                : "rest";
        errorAt(directivePos.begin, JSMSG_STRICT_NON_SIMPLE_PARAMS,
                parameterKind);
        return false;
      }
    }

    // We're going to be in strict mode. Note that this scope explicitly
    // had "use strict";
    pc_->sc()->setExplicitUseStrict();
    if (!pc_->sc()->strict()) {
      // Some strict mode violations can appear before a Use Strict Directive
      // is applied.  (See the |DeprecatedContent| enum initializers.)  These
      // violations can manifest in two ways.
      //
      // First, the violation can appear *before* the Use Strict Directive.
      // Numeric literals (and therefore octal literals) can only precede a
      // Use Strict Directive if this function's parameter list is not simple,
      // but we reported an error for non-simple parameter lists above, so
      // octal literals present no issue.  But octal escapes and \8 and \9 can
      // appear in the directive prologue before a Use Strict Directive:
      //
      //   function f()
      //   {
      //     "hell\157 world";  // octal escape
      //     "\8"; "\9";        // NonOctalDecimalEscape
      //     "use strict";      // retroactively makes all the above errors
      //   }
      //
      // Second, the violation can appear *after* the Use Strict Directive but
      // *before* the directive is recognized as terminated.  This only
      // happens when a directive is terminated by ASI, and the next token
      // contains a violation:
      //
      //   function a()
      //   {
      //     "use strict"  // ASI
      //     0755;
      //   }
      //   function b()
      //   {
      //     "use strict"  // ASI
      //     "hell\157 world";
      //   }
      //   function c()
      //   {
      //     "use strict"  // ASI
      //     "\8";
      //   }
      //
      // We note such violations when tokenizing.  Then, if a violation has
      // been observed at the time a "use strict" is applied, we report the
      // error.
      switch (anyChars.sawDeprecatedContent()) {
        case DeprecatedContent::None:
          break;
        case DeprecatedContent::OctalLiteral:
          error(JSMSG_DEPRECATED_OCTAL_LITERAL);
          return false;
        case DeprecatedContent::OctalEscape:
          error(JSMSG_DEPRECATED_OCTAL_ESCAPE);
          return false;
        case DeprecatedContent::EightOrNineEscape:
          error(JSMSG_DEPRECATED_EIGHT_OR_NINE_ESCAPE);
          return false;
      }

      pc_->sc()->setStrictScript();
    }
  } else if (IsUseAsmDirective(directivePos, directive)) {
    if (pc_->isFunctionBox()) {
      return asmJS(list);
    }
    return warningAt(directivePos.begin, JSMSG_USE_ASM_DIRECTIVE_FAIL);
  }
  return true;
}

template <class ParseHandler, typename Unit>
typename ParseHandler::ListNodeResult
GeneralParser<ParseHandler, Unit>::statementList(YieldHandling yieldHandling) {
  AutoCheckRecursionLimit recursion(this->fc_);
  if (!recursion.check(this->fc_)) {
    return errorResult();
  }

  ListNodeType stmtList;
  MOZ_TRY_VAR(stmtList, handler_.newStatementList(pos()));

  bool canHaveDirectives = pc_->atBodyLevel();
  if (canHaveDirectives) {
    // Clear flags for deprecated content that might have been seen in an
    // enclosing context.
    anyChars.clearSawDeprecatedContent();
  }

  bool canHaveHashbangComment = pc_->atTopLevel();
  if (canHaveHashbangComment) {
    tokenStream.consumeOptionalHashbangComment();
  }

  bool afterReturn = false;
  bool warnedAboutStatementsAfterReturn = false;
  uint32_t statementBegin = 0;
  for (;;) {
    TokenKind tt = TokenKind::Eof;
    if (!tokenStream.peekToken(&tt, TokenStream::SlashIsRegExp)) {
      if (anyChars.isEOF()) {
        isUnexpectedEOF_ = true;
      }
      return errorResult();
    }
    if (tt == TokenKind::Eof || tt == TokenKind::RightCurly) {
      TokenPos pos;
      if (!tokenStream.peekTokenPos(&pos, TokenStream::SlashIsRegExp)) {
        return errorResult();
      }
      handler_.setListEndPosition(stmtList, pos);
      break;
    }
    if (afterReturn) {
      if (!tokenStream.peekOffset(&statementBegin,
                                  TokenStream::SlashIsRegExp)) {
        return errorResult();
      }
    }
    auto nextResult = statementListItem(yieldHandling, canHaveDirectives);
    if (nextResult.isErr()) {
      if (anyChars.isEOF()) {
        isUnexpectedEOF_ = true;
      }
      return errorResult();
    }
    Node next = nextResult.unwrap();
    if (!warnedAboutStatementsAfterReturn) {
      if (afterReturn) {
        if (!handler_.isStatementPermittedAfterReturnStatement(next)) {
          if (!warningAt(statementBegin, JSMSG_STMT_AFTER_RETURN)) {
            return errorResult();
          }

          warnedAboutStatementsAfterReturn = true;
        }
      } else if (handler_.isReturnStatement(next)) {
        afterReturn = true;
      }
    }

    if (canHaveDirectives) {
      if (!maybeParseDirective(stmtList, next, &canHaveDirectives)) {
        return errorResult();
      }
    }

    handler_.addStatementToList(stmtList, next);
  }

  return stmtList;
}

template <class ParseHandler, typename Unit>
typename ParseHandler::NodeResult GeneralParser<ParseHandler, Unit>::condition(
    InHandling inHandling, YieldHandling yieldHandling) {
  if (!mustMatchToken(TokenKind::LeftParen, JSMSG_PAREN_BEFORE_COND)) {
    return errorResult();
  }

  Node pn;
  MOZ_TRY_VAR(pn, exprInParens(inHandling, yieldHandling, TripledotProhibited));

  if (!mustMatchToken(TokenKind::RightParen, JSMSG_PAREN_AFTER_COND)) {
    return errorResult();
  }

  return pn;
}

template <class ParseHandler, typename Unit>
bool GeneralParser<ParseHandler, Unit>::matchLabel(
    YieldHandling yieldHandling, TaggedParserAtomIndex* labelOut) {
  MOZ_ASSERT(labelOut != nullptr);
  TokenKind tt = TokenKind::Eof;
  if (!tokenStream.peekTokenSameLine(&tt, TokenStream::SlashIsRegExp)) {
    return false;
  }

  if (TokenKindIsPossibleIdentifier(tt)) {
    tokenStream.consumeKnownToken(tt, TokenStream::SlashIsRegExp);

    *labelOut = labelIdentifier(yieldHandling);
    if (!*labelOut) {
      return false;
    }
  } else {
    *labelOut = TaggedParserAtomIndex::null();
  }
  return true;
}

template <class ParseHandler, typename Unit>
GeneralParser<ParseHandler, Unit>::PossibleError::PossibleError(
    GeneralParser<ParseHandler, Unit>& parser)
    : parser_(parser) {}

template <class ParseHandler, typename Unit>
typename GeneralParser<ParseHandler, Unit>::PossibleError::Error&
GeneralParser<ParseHandler, Unit>::PossibleError::error(ErrorKind kind) {
  if (kind == ErrorKind::Expression) {
    return exprError_;
  }
  if (kind == ErrorKind::Destructuring) {
    return destructuringError_;
  }
  MOZ_ASSERT(kind == ErrorKind::DestructuringWarning);
  return destructuringWarning_;
}

template <class ParseHandler, typename Unit>
void GeneralParser<ParseHandler, Unit>::PossibleError::setResolved(
    ErrorKind kind) {
  error(kind).state_ = ErrorState::None;
}

template <class ParseHandler, typename Unit>
bool GeneralParser<ParseHandler, Unit>::PossibleError::hasError(
    ErrorKind kind) {
  return error(kind).state_ == ErrorState::Pending;
}

template <class ParseHandler, typename Unit>
bool GeneralParser<ParseHandler,
                   Unit>::PossibleError::hasPendingDestructuringError() {
  return hasError(ErrorKind::Destructuring);
}

template <class ParseHandler, typename Unit>
void GeneralParser<ParseHandler, Unit>::PossibleError::setPending(
    ErrorKind kind, const TokenPos& pos, unsigned errorNumber) {
  // Don't overwrite a previously recorded error.
  if (hasError(kind)) {
    return;
  }

  // If we report an error later, we'll do it from the position where we set
  // the state to pending.
  Error& err = error(kind);
  err.offset_ = pos.begin;
  err.errorNumber_ = errorNumber;
  err.state_ = ErrorState::Pending;
}

template <class ParseHandler, typename Unit>
void GeneralParser<ParseHandler, Unit>::PossibleError::
    setPendingDestructuringErrorAt(const TokenPos& pos, unsigned errorNumber) {
  setPending(ErrorKind::Destructuring, pos, errorNumber);
}

template <class ParseHandler, typename Unit>
void GeneralParser<ParseHandler, Unit>::PossibleError::
    setPendingDestructuringWarningAt(const TokenPos& pos,
                                     unsigned errorNumber) {
  setPending(ErrorKind::DestructuringWarning, pos, errorNumber);
}

template <class ParseHandler, typename Unit>
void GeneralParser<ParseHandler, Unit>::PossibleError::
    setPendingExpressionErrorAt(const TokenPos& pos, unsigned errorNumber) {
  setPending(ErrorKind::Expression, pos, errorNumber);
}

template <class ParseHandler, typename Unit>
bool GeneralParser<ParseHandler, Unit>::PossibleError::checkForError(
    ErrorKind kind) {
  if (!hasError(kind)) {
    return true;
  }

  Error& err = error(kind);
  parser_.errorAt(err.offset_, err.errorNumber_);
  return false;
}

template <class ParseHandler, typename Unit>
bool GeneralParser<ParseHandler,
                   Unit>::PossibleError::checkForDestructuringErrorOrWarning() {
  // Clear pending expression error, because we're definitely not in an
  // expression context.
  setResolved(ErrorKind::Expression);

  // Report any pending destructuring error.
  return checkForError(ErrorKind::Destructuring);
}

template <class ParseHandler, typename Unit>
bool GeneralParser<ParseHandler,
                   Unit>::PossibleError::checkForExpressionError() {
  // Clear pending destructuring error, because we're definitely not
  // in a destructuring context.
  setResolved(ErrorKind::Destructuring);
  setResolved(ErrorKind::DestructuringWarning);

  // Report any pending expression error.
  return checkForError(ErrorKind::Expression);
}

template <class ParseHandler, typename Unit>
void GeneralParser<ParseHandler, Unit>::PossibleError::transferErrorTo(
    ErrorKind kind, PossibleError* other) {
  if (hasError(kind) && !other->hasError(kind)) {
    Error& err = error(kind);
    Error& otherErr = other->error(kind);
    otherErr.offset_ = err.offset_;
    otherErr.errorNumber_ = err.errorNumber_;
    otherErr.state_ = err.state_;
  }
}

template <class ParseHandler, typename Unit>
void GeneralParser<ParseHandler, Unit>::PossibleError::transferErrorsTo(
    PossibleError* other) {
  MOZ_ASSERT(other);
  MOZ_ASSERT(this != other);
  MOZ_ASSERT(&parser_ == &other->parser_,
             "Can't transfer fields to an instance which belongs to a "
             "different parser");

  transferErrorTo(ErrorKind::Destructuring, other);
  transferErrorTo(ErrorKind::Expression, other);
}

template <class ParseHandler, typename Unit>
typename ParseHandler::BinaryNodeResult
GeneralParser<ParseHandler, Unit>::bindingInitializer(
    Node lhs, DeclarationKind kind, YieldHandling yieldHandling) {
  MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::Assign));

  if (kind == DeclarationKind::FormalParameter) {
    pc_->functionBox()->hasParameterExprs = true;
  }

  Node rhs;
  MOZ_TRY_VAR(rhs, assignExpr(InAllowed, yieldHandling, TripledotProhibited));

  BinaryNodeType assign;
  MOZ_TRY_VAR(assign,
              handler_.newAssignment(ParseNodeKind::AssignExpr, lhs, rhs));

  return assign;
}

template <class ParseHandler, typename Unit>
typename ParseHandler::NameNodeResult
GeneralParser<ParseHandler, Unit>::bindingIdentifier(
    DeclarationKind kind, YieldHandling yieldHandling) {
  TaggedParserAtomIndex name = bindingIdentifier(yieldHandling);
  if (!name) {
    return errorResult();
  }

  NameNodeType binding;
  MOZ_TRY_VAR(binding, newName(name));
  if (!noteDeclaredName(name, kind, pos())) {
    return errorResult();
  }

  return binding;
}

template <class ParseHandler, typename Unit>
typename ParseHandler::NodeResult
GeneralParser<ParseHandler, Unit>::bindingIdentifierOrPattern(
    DeclarationKind kind, YieldHandling yieldHandling, TokenKind tt) {
  if (tt == TokenKind::LeftBracket) {
    return arrayBindingPattern(kind, yieldHandling);
  }

  if (tt == TokenKind::LeftCurly) {
    return objectBindingPattern(kind, yieldHandling);
  }

  if (!TokenKindIsPossibleIdentifierName(tt)) {
    error(JSMSG_NO_VARIABLE_NAME);
    return errorResult();
  }

  return bindingIdentifier(kind, yieldHandling);
}

template <class ParseHandler, typename Unit>
typename ParseHandler::ListNodeResult
GeneralParser<ParseHandler, Unit>::objectBindingPattern(
    DeclarationKind kind, YieldHandling yieldHandling) {
  MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::LeftCurly));

  AutoCheckRecursionLimit recursion(this->fc_);
  if (!recursion.check(this->fc_)) {
    return errorResult();
  }

  uint32_t begin = pos().begin;
  ListNodeType literal;
  MOZ_TRY_VAR(literal, handler_.newObjectLiteral(begin));

  Maybe<DeclarationKind> declKind = Some(kind);
  TaggedParserAtomIndex propAtom;
  for (;;) {
    TokenKind tt;
    if (!tokenStream.peekToken(&tt)) {
      return errorResult();
    }
    if (tt == TokenKind::RightCurly) {
      break;
    }

    if (tt == TokenKind::TripleDot) {
      tokenStream.consumeKnownToken(TokenKind::TripleDot);
      uint32_t begin = pos().begin;

      TokenKind tt;
      if (!tokenStream.getToken(&tt)) {
        return errorResult();
      }

      if (!TokenKindIsPossibleIdentifierName(tt)) {
        error(JSMSG_NO_VARIABLE_NAME);
        return errorResult();
      }

      NameNodeType inner;
      MOZ_TRY_VAR(inner, bindingIdentifier(kind, yieldHandling));

      if (!handler_.addSpreadProperty(literal, begin, inner)) {
        return errorResult();
      }
    } else {
      TokenPos namePos = anyChars.nextToken().pos;

      PropertyType propType;
      Node propName;
      MOZ_TRY_VAR(propName, propertyOrMethodName(
                                yieldHandling, PropertyNameInPattern, declKind,
                                literal, &propType, &propAtom));

      if (propType == PropertyType::Normal) {
        // Handle e.g., |var {p: x} = o| and |var {p: x=0} = o|.

        if (!tokenStream.getToken(&tt, TokenStream::SlashIsRegExp)) {
          return errorResult();
        }

        Node binding;
        MOZ_TRY_VAR(binding,
                    bindingIdentifierOrPattern(kind, yieldHandling, tt));

        bool hasInitializer;
        if (!tokenStream.matchToken(&hasInitializer, TokenKind::Assign,
                                    TokenStream::SlashIsRegExp)) {
          return errorResult();
        }

        Node bindingExpr;
        if (hasInitializer) {
          MOZ_TRY_VAR(bindingExpr,
                      bindingInitializer(binding, kind, yieldHandling));
        } else {
          bindingExpr = binding;
        }

        if (!handler_.addPropertyDefinition(literal, propName, bindingExpr)) {
          return errorResult();
        }
      } else if (propType == PropertyType::Shorthand) {
        // Handle e.g., |var {x, y} = o| as destructuring shorthand
        // for |var {x: x, y: y} = o|.
        MOZ_ASSERT(TokenKindIsPossibleIdentifierName(tt));

        NameNodeType binding;
        MOZ_TRY_VAR(binding, bindingIdentifier(kind, yieldHandling));

        if (!handler_.addShorthand(literal, handler_.asNameNode(propName),
                                   binding)) {
          return errorResult();
        }
      } else if (propType == PropertyType::CoverInitializedName) {
        // Handle e.g., |var {x=1, y=2} = o| as destructuring
        // shorthand with default values.
        MOZ_ASSERT(TokenKindIsPossibleIdentifierName(tt));

        NameNodeType binding;
        MOZ_TRY_VAR(binding, bindingIdentifier(kind, yieldHandling));

        tokenStream.consumeKnownToken(TokenKind::Assign);

        BinaryNodeType bindingExpr;
        MOZ_TRY_VAR(bindingExpr,
                    bindingInitializer(binding, kind, yieldHandling));

        if (!handler_.addPropertyDefinition(literal, propName, bindingExpr)) {
          return errorResult();
        }
      } else {
        errorAt(namePos.begin, JSMSG_NO_VARIABLE_NAME);
        return errorResult();
      }
    }

    bool matched;
    if (!tokenStream.matchToken(&matched, TokenKind::Comma,
                                TokenStream::SlashIsInvalid)) {
      return errorResult();
    }
    if (!matched) {
      break;
    }
    if (tt == TokenKind::TripleDot) {
      error(JSMSG_REST_WITH_COMMA);
      return errorResult();
    }
  }

  if (!mustMatchToken(TokenKind::RightCurly, [this, begin](TokenKind actual) {
        this->reportMissingClosing(JSMSG_CURLY_AFTER_LIST, JSMSG_CURLY_OPENED,
                                   begin);
      })) {
    return errorResult();
  }

  handler_.setEndPosition(literal, pos().end);
  return literal;
}

template <class ParseHandler, typename Unit>
typename ParseHandler::ListNodeResult
GeneralParser<ParseHandler, Unit>::arrayBindingPattern(
    DeclarationKind kind, YieldHandling yieldHandling) {
  MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::LeftBracket));

  AutoCheckRecursionLimit recursion(this->fc_);
  if (!recursion.check(this->fc_)) {
    return errorResult();
  }

  uint32_t begin = pos().begin;
  ListNodeType literal;
  MOZ_TRY_VAR(literal, handler_.newArrayLiteral(begin));

  uint32_t index = 0;
  for (;; index++) {
    if (index >= NativeObject::MAX_DENSE_ELEMENTS_COUNT) {
      error(JSMSG_ARRAY_INIT_TOO_BIG);
      return errorResult();
    }

    TokenKind tt;
    if (!tokenStream.getToken(&tt)) {
      return errorResult();
    }

    if (tt == TokenKind::RightBracket) {
      anyChars.ungetToken();
      break;
    }

    if (tt == TokenKind::Comma) {
      if (!handler_.addElision(literal, pos())) {
        return errorResult();
      }
    } else if (tt == TokenKind::TripleDot) {
      uint32_t begin = pos().begin;

      TokenKind tt;
      if (!tokenStream.getToken(&tt)) {
        return errorResult();
      }

      Node inner;
      MOZ_TRY_VAR(inner, bindingIdentifierOrPattern(kind, yieldHandling, tt));

      if (!handler_.addSpreadElement(literal, begin, inner)) {
        return errorResult();
      }
    } else {
      Node binding;
      MOZ_TRY_VAR(binding, bindingIdentifierOrPattern(kind, yieldHandling, tt));

      bool hasInitializer;
      if (!tokenStream.matchToken(&hasInitializer, TokenKind::Assign,
                                  TokenStream::SlashIsRegExp)) {
        return errorResult();
      }

      Node element;
      if (hasInitializer) {
        MOZ_TRY_VAR(element, bindingInitializer(binding, kind, yieldHandling));
      } else {
        element = binding;
      }

      handler_.addArrayElement(literal, element);
    }

    if (tt != TokenKind::Comma) {
      // If we didn't already match TokenKind::Comma in above case.
      bool matched;
      if (!tokenStream.matchToken(&matched, TokenKind::Comma,
                                  TokenStream::SlashIsRegExp)) {
        return errorResult();
      }
      if (!matched) {
        break;
      }

      if (tt == TokenKind::TripleDot) {
        error(JSMSG_REST_WITH_COMMA);
        return errorResult();
      }
    }
  }

  if (!mustMatchToken(TokenKind::RightBracket, [this, begin](TokenKind actual) {
        this->reportMissingClosing(JSMSG_BRACKET_AFTER_LIST,
                                   JSMSG_BRACKET_OPENED, begin);
      })) {
    return errorResult();
  }

  handler_.setEndPosition(literal, pos().end);
  return literal;
}

template <class ParseHandler, typename Unit>
typename ParseHandler::NodeResult
GeneralParser<ParseHandler, Unit>::destructuringDeclaration(
    DeclarationKind kind, YieldHandling yieldHandling, TokenKind tt) {
  MOZ_ASSERT(anyChars.isCurrentTokenType(tt));
  MOZ_ASSERT(tt == TokenKind::LeftBracket || tt == TokenKind::LeftCurly);

  if (tt == TokenKind::LeftBracket) {
    return arrayBindingPattern(kind, yieldHandling);
  }
  return objectBindingPattern(kind, yieldHandling);
}

template <class ParseHandler, typename Unit>
typename ParseHandler::NodeResult
GeneralParser<ParseHandler, Unit>::destructuringDeclarationWithoutYieldOrAwait(
    DeclarationKind kind, YieldHandling yieldHandling, TokenKind tt) {
  uint32_t startYieldOffset = pc_->lastYieldOffset;
  uint32_t startAwaitOffset = pc_->lastAwaitOffset;

  Node res;
  MOZ_TRY_VAR(res, destructuringDeclaration(kind, yieldHandling, tt));

  if (pc_->lastYieldOffset != startYieldOffset) {
    errorAt(pc_->lastYieldOffset, JSMSG_YIELD_IN_PARAMETER);
    return errorResult();
  }
  if (pc_->lastAwaitOffset != startAwaitOffset) {
    errorAt(pc_->lastAwaitOffset, JSMSG_AWAIT_IN_PARAMETER);
    return errorResult();
  }
  return res;
}

template <class ParseHandler, typename Unit>
typename ParseHandler::LexicalScopeNodeResult
GeneralParser<ParseHandler, Unit>::blockStatement(YieldHandling yieldHandling,
                                                  unsigned errorNumber) {
  MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::LeftCurly));
  uint32_t openedPos = pos().begin;

  ParseContext::Statement stmt(pc_, StatementKind::Block);
  ParseContext::Scope scope(this);
  if (!scope.init(pc_)) {
    return errorResult();
  }

  ListNodeType list;
  MOZ_TRY_VAR(list, statementList(yieldHandling));

  if (!mustMatchToken(TokenKind::RightCurly, [this, errorNumber,
                                              openedPos](TokenKind actual) {
        this->reportMissingClosing(errorNumber, JSMSG_CURLY_OPENED, openedPos);
      })) {
    return errorResult();
  }

  return finishLexicalScope(scope, list);
}

template <class ParseHandler, typename Unit>
typename ParseHandler::NodeResult
GeneralParser<ParseHandler, Unit>::expressionAfterForInOrOf(
    ParseNodeKind forHeadKind, YieldHandling yieldHandling) {
  MOZ_ASSERT(forHeadKind == ParseNodeKind::ForIn ||
             forHeadKind == ParseNodeKind::ForOf);
  if (forHeadKind == ParseNodeKind::ForOf) {
    return assignExpr(InAllowed, yieldHandling, TripledotProhibited);
  }

  return expr(InAllowed, yieldHandling, TripledotProhibited);
}

template <class ParseHandler, typename Unit>
typename ParseHandler::NodeResult
GeneralParser<ParseHandler, Unit>::declarationPattern(
    DeclarationKind declKind, TokenKind tt, bool initialDeclaration,
    YieldHandling yieldHandling, ParseNodeKind* forHeadKind,
    Node* forInOrOfExpression) {
  MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::LeftBracket) ||
             anyChars.isCurrentTokenType(TokenKind::LeftCurly));

  Node pattern;
  MOZ_TRY_VAR(pattern, destructuringDeclaration(declKind, yieldHandling, tt));

  if (initialDeclaration && forHeadKind) {
    bool isForIn, isForOf;
    if (!matchInOrOf(&isForIn, &isForOf)) {
      return errorResult();
    }

    if (isForIn) {
      *forHeadKind = ParseNodeKind::ForIn;
    } else if (isForOf) {
      *forHeadKind = ParseNodeKind::ForOf;
    } else {
      *forHeadKind = ParseNodeKind::ForHead;
    }

    if (*forHeadKind != ParseNodeKind::ForHead) {
      MOZ_TRY_VAR(*forInOrOfExpression,
                  expressionAfterForInOrOf(*forHeadKind, yieldHandling));

      return pattern;
    }
  }

  if (!mustMatchToken(TokenKind::Assign, JSMSG_BAD_DESTRUCT_DECL)) {
    return errorResult();
  }

  Node init;
  MOZ_TRY_VAR(init, assignExpr(forHeadKind ? InProhibited : InAllowed,
                               yieldHandling, TripledotProhibited));

  return handler_.newAssignment(ParseNodeKind::AssignExpr, pattern, init);
}

template <class ParseHandler, typename Unit>
typename ParseHandler::AssignmentNodeResult
GeneralParser<ParseHandler, Unit>::initializerInNameDeclaration(
    NameNodeType binding, DeclarationKind declKind, bool initialDeclaration,
    YieldHandling yieldHandling, ParseNodeKind* forHeadKind,
    Node* forInOrOfExpression) {
  MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::Assign));

  uint32_t initializerOffset;
  if (!tokenStream.peekOffset(&initializerOffset, TokenStream::SlashIsRegExp)) {
    return errorResult();
  }

  Node initializer;
  MOZ_TRY_VAR(initializer, assignExpr(forHeadKind ? InProhibited : InAllowed,
                                      yieldHandling, TripledotProhibited));

  if (forHeadKind && initialDeclaration) {
    bool isForIn, isForOf;
    if (!matchInOrOf(&isForIn, &isForOf)) {
      return errorResult();
    }

    // An initialized declaration can't appear in a for-of:
    //
    //   for (var/let/const x = ... of ...); // BAD
    if (isForOf) {
      errorAt(initializerOffset, JSMSG_OF_AFTER_FOR_LOOP_DECL);
      return errorResult();
    }

    if (isForIn) {
      // Lexical declarations in for-in loops can't be initialized:
      //
      //   for (let/const x = ... in ...); // BAD
      if (DeclarationKindIsLexical(declKind)) {
        errorAt(initializerOffset, JSMSG_IN_AFTER_LEXICAL_FOR_DECL);
        return errorResult();
      }

      // This leaves only initialized for-in |var| declarations.  ES6
      // forbids these; later ES un-forbids in non-strict mode code.
      *forHeadKind = ParseNodeKind::ForIn;
      if (!strictModeErrorAt(initializerOffset,
                             JSMSG_INVALID_FOR_IN_DECL_WITH_INIT)) {
        return errorResult();
      }

      MOZ_TRY_VAR(
          *forInOrOfExpression,
          expressionAfterForInOrOf(ParseNodeKind::ForIn, yieldHandling));
    } else {
      *forHeadKind = ParseNodeKind::ForHead;
    }
  }

  return handler_.finishInitializerAssignment(binding, initializer);
}

template <class ParseHandler, typename Unit>
typename ParseHandler::NodeResult
GeneralParser<ParseHandler, Unit>::declarationName(DeclarationKind declKind,
                                                   TokenKind tt,
                                                   bool initialDeclaration,
                                                   YieldHandling yieldHandling,
                                                   ParseNodeKind* forHeadKind,
                                                   Node* forInOrOfExpression) {
  // Anything other than possible identifier is an error.
  if (!TokenKindIsPossibleIdentifier(tt)) {
    error(JSMSG_NO_VARIABLE_NAME);
    return errorResult();
  }

  TaggedParserAtomIndex name = bindingIdentifier(yieldHandling);
  if (!name) {
    return errorResult();
  }

  NameNodeType binding;
  MOZ_TRY_VAR(binding, newName(name));

  TokenPos namePos = pos();

  // The '=' context after a variable name in a declaration is an opportunity
  // for ASI, and thus for the next token to start an ExpressionStatement:
  //
  //  var foo   // VariableDeclaration
  //  /bar/g;   // ExpressionStatement
  //
  // Therefore get the token here with SlashIsRegExp.
  bool matched;
  if (!tokenStream.matchToken(&matched, TokenKind::Assign,
                              TokenStream::SlashIsRegExp)) {
    return errorResult();
  }

  Node declaration;
  if (matched) {
    MOZ_TRY_VAR(declaration,
                initializerInNameDeclaration(binding, declKind,
                                             initialDeclaration, yieldHandling,
                                             forHeadKind, forInOrOfExpression));
  } else {
    declaration = binding;

    if (initialDeclaration && forHeadKind) {
      bool isForIn, isForOf;
      if (!matchInOrOf(&isForIn, &isForOf)) {
        return errorResult();
      }

      if (isForIn) {
        *forHeadKind = ParseNodeKind::ForIn;
#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
        if (declKind == DeclarationKind::Using ||
            declKind == DeclarationKind::AwaitUsing) {
          errorAt(namePos.begin, JSMSG_NO_IN_WITH_USING);
          return errorResult();
        }
#endif
      } else if (isForOf) {
        *forHeadKind = ParseNodeKind::ForOf;
      } else {
        *forHeadKind = ParseNodeKind::ForHead;
      }
    }

    if (forHeadKind && *forHeadKind != ParseNodeKind::ForHead) {
      MOZ_TRY_VAR(*forInOrOfExpression,
                  expressionAfterForInOrOf(*forHeadKind, yieldHandling));
    } else {
      // Normal const declarations, and const declarations in for(;;)
      // heads, must be initialized.
      if (declKind == DeclarationKind::Const) {
        errorAt(namePos.begin, JSMSG_BAD_CONST_DECL);
        return errorResult();
      }
    }
  }

  // Note the declared name after knowing whether or not we are in a for-of
  // loop, due to special early error semantics in Annex B.3.5.
  if (!noteDeclaredName(name, declKind, namePos)) {
    return errorResult();
  }

  return declaration;
}

template <class ParseHandler, typename Unit>
typename ParseHandler::DeclarationListNodeResult
GeneralParser<ParseHandler, Unit>::declarationList(
    YieldHandling yieldHandling, ParseNodeKind kind,
    ParseNodeKind* forHeadKind /* = nullptr */,
    Node* forInOrOfExpression /* = nullptr */) {
  MOZ_ASSERT(kind == ParseNodeKind::VarStmt || kind == ParseNodeKind::LetDecl ||
             kind == ParseNodeKind::ConstDecl
#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
             || kind == ParseNodeKind::UsingDecl ||
             kind == ParseNodeKind::AwaitUsingDecl
#endif
  );

  DeclarationKind declKind;
  switch (kind) {
    case ParseNodeKind::VarStmt:
      declKind = DeclarationKind::Var;
      break;
    case ParseNodeKind::ConstDecl:
      declKind = DeclarationKind::Const;
      break;
    case ParseNodeKind::LetDecl:
      declKind = DeclarationKind::Let;
      break;
#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
    case ParseNodeKind::UsingDecl:
      declKind = DeclarationKind::Using;
      break;
    case ParseNodeKind::AwaitUsingDecl:
      declKind = DeclarationKind::AwaitUsing;
      break;
#endif
    default:
      MOZ_CRASH("Unknown declaration kind");
  }

  DeclarationListNodeType decl;
  MOZ_TRY_VAR(decl, handler_.newDeclarationList(kind, pos()));

  bool moreDeclarations;
  bool initialDeclaration = true;
  do {
    MOZ_ASSERT_IF(!initialDeclaration && forHeadKind,
                  *forHeadKind == ParseNodeKind::ForHead);

    TokenKind tt;
    if (!tokenStream.getToken(&tt)) {
      return errorResult();
    }

    Node binding;
    if (tt == TokenKind::LeftBracket || tt == TokenKind::LeftCurly) {
      MOZ_TRY_VAR(binding, declarationPattern(declKind, tt, initialDeclaration,
                                              yieldHandling, forHeadKind,
                                              forInOrOfExpression));
    } else {
      MOZ_TRY_VAR(binding, declarationName(declKind, tt, initialDeclaration,
                                           yieldHandling, forHeadKind,
                                           forInOrOfExpression));
    }

    handler_.addList(decl, binding);

    // If we have a for-in/of loop, the above call matches the entirety
    // of the loop head (up to the closing parenthesis).
    if (forHeadKind && *forHeadKind != ParseNodeKind::ForHead) {
      break;
    }

    initialDeclaration = false;

    if (!tokenStream.matchToken(&moreDeclarations, TokenKind::Comma,
                                TokenStream::SlashIsRegExp)) {
      return errorResult();
    }
  } while (moreDeclarations);

  return decl;
}

template <class ParseHandler, typename Unit>
typename ParseHandler::DeclarationListNodeResult
GeneralParser<ParseHandler, Unit>::lexicalDeclaration(
    YieldHandling yieldHandling, DeclarationKind kind) {
  MOZ_ASSERT(kind == DeclarationKind::Const || kind == DeclarationKind::Let
#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
             || kind == DeclarationKind::Using ||
             kind == DeclarationKind::AwaitUsing
#endif
  );

  if (options().selfHostingMode) {
    error(JSMSG_SELFHOSTED_LEXICAL);
    return errorResult();
  }

  /*
   * Parse body-level lets without a new block object. ES6 specs
   * that an execution environment's initial lexical environment
   * is the VariableEnvironment, i.e., body-level lets are in
   * the same environment record as vars.
   *
   * However, they cannot be parsed exactly as vars, as ES6
   * requires that uninitialized lets throw ReferenceError on use.
   *
   * See 8.1.1.1.6 and the note in 13.2.1.
   */
  DeclarationListNodeType decl;
  ParseNodeKind pnk;
  switch (kind) {
    case DeclarationKind::Const:
      pnk = ParseNodeKind::ConstDecl;
      break;
#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
    case DeclarationKind::Using:
      pnk = ParseNodeKind::UsingDecl;
      break;
    case DeclarationKind::AwaitUsing:
      pnk = ParseNodeKind::AwaitUsingDecl;
      break;
#endif
    case DeclarationKind::Let:
      pnk = ParseNodeKind::LetDecl;
      break;
    default:
      MOZ_CRASH("unexpected node kind");
  }
  MOZ_TRY_VAR(decl, declarationList(yieldHandling, pnk));
  if (!matchOrInsertSemicolon()) {
    return errorResult();
  }

  return decl;
}

template <class ParseHandler, typename Unit>
typename ParseHandler::NameNodeResult
GeneralParser<ParseHandler, Unit>::moduleExportName() {
  MOZ_ASSERT(anyChars.currentToken().type == TokenKind::String);
  TaggedParserAtomIndex name = anyChars.currentToken().atom();
  if (!this->parserAtoms().isModuleExportName(name)) {
    error(JSMSG_UNPAIRED_SURROGATE_EXPORT);
    return errorResult();
  }
  return handler_.newStringLiteral(name, pos());
}

template <class ParseHandler, typename Unit>
bool GeneralParser<ParseHandler, Unit>::withClause(ListNodeType attributesSet) {
  MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::Assert) ||
             anyChars.isCurrentTokenType(TokenKind::With));

  if (!options().importAttributes()) {
    error(JSMSG_IMPORT_ASSERTIONS_NOT_SUPPORTED);
    return false;
  }

  if (!abortIfSyntaxParser()) {
    return false;
  }

  if (!mustMatchToken(TokenKind::LeftCurly, JSMSG_CURLY_AFTER_ASSERT)) {
    return false;
  }

  // Handle the form |... assert {}|
  TokenKind token;
  if (!tokenStream.getToken(&token)) {
    return false;
  }
  if (token == TokenKind::RightCurly) {
    return true;
  }

  js::HashSet<TaggedParserAtomIndex, TaggedParserAtomIndexHasher,
              js::SystemAllocPolicy>
      usedAssertionKeys;

  for (;;) {
    TaggedParserAtomIndex keyName;
    if (TokenKindIsPossibleIdentifierName(token)) {
      keyName = anyChars.currentName();
    } else if (token == TokenKind::String) {
      keyName = anyChars.currentToken().atom();
    } else {
      error(JSMSG_ASSERT_KEY_EXPECTED);
      return false;
    }

    auto p = usedAssertionKeys.lookupForAdd(keyName);
    if (p) {
      UniqueChars str = this->parserAtoms().toPrintableString(keyName);
      if (!str) {
        ReportOutOfMemory(this->fc_);
        return false;
      }
      error(JSMSG_DUPLICATE_ASSERT_KEY, str.get());
      return false;
    }
    if (!usedAssertionKeys.add(p, keyName)) {
      ReportOutOfMemory(this->fc_);
      return false;
    }

    NameNodeType keyNode;
    MOZ_TRY_VAR_OR_RETURN(keyNode, newName(keyName), false);

    if (!mustMatchToken(TokenKind::Colon, JSMSG_COLON_AFTER_ASSERT_KEY)) {
      return false;
    }
    if (!mustMatchToken(TokenKind::String, JSMSG_ASSERT_STRING_LITERAL)) {
      return false;
    }

    NameNodeType valueNode;
    MOZ_TRY_VAR_OR_RETURN(valueNode, stringLiteral(), false);

    BinaryNodeType importAttributeNode;
    MOZ_TRY_VAR_OR_RETURN(importAttributeNode,
                          handler_.newImportAttribute(keyNode, valueNode),
                          false);

    handler_.addList(attributesSet, importAttributeNode);

    if (!tokenStream.getToken(&token)) {
      return false;
    }
    if (token == TokenKind::Comma) {
      if (!tokenStream.getToken(&token)) {
        return false;
      }
    }
    if (token == TokenKind::RightCurly) {
      break;
    }
  }

  return true;
}

template <class ParseHandler, typename Unit>
bool GeneralParser<ParseHandler, Unit>::namedImports(
    ListNodeType importSpecSet) {
  if (!abortIfSyntaxParser()) {
    return false;
  }

  while (true) {
    // Handle the forms |import {} from 'a'| and
    // |import { ..., } from 'a'| (where ... is non empty), by
    // escaping the loop early if the next token is }.
    TokenKind tt;
    if (!tokenStream.getToken(&tt)) {
      return false;
    }

    if (tt == TokenKind::RightCurly) {
      break;
    }

    TaggedParserAtomIndex importName;
    NameNodeType importNameNode = null();
    if (TokenKindIsPossibleIdentifierName(tt)) {
      importName = anyChars.currentName();
      MOZ_TRY_VAR_OR_RETURN(importNameNode, newName(importName), false);
    } else if (tt == TokenKind::String) {
      MOZ_TRY_VAR_OR_RETURN(importNameNode, moduleExportName(), false);
    } else {
      error(JSMSG_NO_IMPORT_NAME);
      return false;
    }

    bool matched;
    if (!tokenStream.matchToken(&matched, TokenKind::As)) {
      return false;
    }

    if (matched) {
      TokenKind afterAs;
      if (!tokenStream.getToken(&afterAs)) {
        return false;
      }

      if (!TokenKindIsPossibleIdentifierName(afterAs)) {
        error(JSMSG_NO_BINDING_NAME);
        return false;
      }
    } else {
      // String export names can't refer to local bindings.
      if (tt == TokenKind::String) {
        error(JSMSG_AS_AFTER_STRING);
        return false;
      }

      // Keywords cannot be bound to themselves, so an import name
      // that is a keyword is a syntax error if it is not followed
      // by the keyword 'as'.
      // See the ImportSpecifier production in ES6 section 15.2.2.
      MOZ_ASSERT(importName);
      if (IsKeyword(importName)) {
        error(JSMSG_AS_AFTER_RESERVED_WORD, ReservedWordToCharZ(importName));
        return false;
      }
    }

    TaggedParserAtomIndex bindingAtom = importedBinding();
    if (!bindingAtom) {
      return false;
    }

    NameNodeType bindingName;
    MOZ_TRY_VAR_OR_RETURN(bindingName, newName(bindingAtom), false);
    if (!noteDeclaredName(bindingAtom, DeclarationKind::Import, pos())) {
      return false;
    }

    BinaryNodeType importSpec;
    MOZ_TRY_VAR_OR_RETURN(
        importSpec, handler_.newImportSpec(importNameNode, bindingName), false);

    handler_.addList(importSpecSet, importSpec);

    TokenKind next;
    if (!tokenStream.getToken(&next)) {
      return false;
    }

    if (next == TokenKind::RightCurly) {
      break;
    }

    if (next != TokenKind::Comma) {
      error(JSMSG_RC_AFTER_IMPORT_SPEC_LIST);
      return false;
    }
  }

  return true;
}

template <class ParseHandler, typename Unit>
bool GeneralParser<ParseHandler, Unit>::namespaceImport(
    ListNodeType importSpecSet) {
  if (!abortIfSyntaxParser()) {
    return false;
  }

  if (!mustMatchToken(TokenKind::As, JSMSG_AS_AFTER_IMPORT_STAR)) {
    return false;
  }
  uint32_t begin = pos().begin;

  if (!mustMatchToken(TokenKindIsPossibleIdentifierName,
                      JSMSG_NO_BINDING_NAME)) {
    return false;
  }

  // Namespace imports are not indirect bindings but lexical
  // definitions that hold a module namespace object. They are treated
  // as const variables which are initialized during the
  // ModuleInstantiate step.
  TaggedParserAtomIndex bindingName = importedBinding();
  if (!bindingName) {
    return false;
  }
  NameNodeType bindingNameNode;
  MOZ_TRY_VAR_OR_RETURN(bindingNameNode, newName(bindingName), false);
  if (!noteDeclaredName(bindingName, DeclarationKind::Const, pos())) {
    return false;
  }

  // The namespace import name is currently required to live on the
  // environment.
  pc_->varScope().lookupDeclaredName(bindingName)->value()->setClosedOver();

  UnaryNodeType importSpec;
  MOZ_TRY_VAR_OR_RETURN(importSpec,
                        handler_.newImportNamespaceSpec(begin, bindingNameNode),
                        false);

  handler_.addList(importSpecSet, importSpec);

  return true;
}

template <class ParseHandler, typename Unit>
typename ParseHandler::BinaryNodeResult
GeneralParser<ParseHandler, Unit>::importDeclaration() {
  if (!abortIfSyntaxParser()) {
    return errorResult();
  }

  MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::Import));

  if (!pc_->atModuleLevel()) {
    error(JSMSG_IMPORT_DECL_AT_TOP_LEVEL);
    return errorResult();
  }

  uint32_t begin = pos().begin;
  TokenKind tt;
  if (!tokenStream.getToken(&tt)) {
    return errorResult();
  }

  ListNodeType importSpecSet;
  MOZ_TRY_VAR(importSpecSet,
              handler_.newList(ParseNodeKind::ImportSpecList, pos()));

  if (tt == TokenKind::String) {
    // Handle the form |import 'a'| by leaving the list empty. This is
    // equivalent to |import {} from 'a'|.
    handler_.setEndPosition(importSpecSet, pos().begin);
  } else {
    if (tt == TokenKind::LeftCurly) {
      if (!namedImports(importSpecSet)) {
        return errorResult();
      }
    } else if (tt == TokenKind::Mul) {
      if (!namespaceImport(importSpecSet)) {
        return errorResult();
      }
    } else if (TokenKindIsPossibleIdentifierName(tt)) {
      // Handle the form |import a from 'b'|, by adding a single import
      // specifier to the list, with 'default' as the import name and
      // 'a' as the binding name. This is equivalent to
      // |import { default as a } from 'b'|.
      NameNodeType importName;
      MOZ_TRY_VAR(importName,
                  newName(TaggedParserAtomIndex::WellKnown::default_()));

      TaggedParserAtomIndex bindingAtom = importedBinding();
      if (!bindingAtom) {
        return errorResult();
      }

      NameNodeType bindingName;
      MOZ_TRY_VAR(bindingName, newName(bindingAtom));

      if (!noteDeclaredName(bindingAtom, DeclarationKind::Import, pos())) {
        return errorResult();
      }

      BinaryNodeType importSpec;
      MOZ_TRY_VAR(importSpec, handler_.newImportSpec(importName, bindingName));

      handler_.addList(importSpecSet, importSpec);

      if (!tokenStream.peekToken(&tt)) {
        return errorResult();
      }

      if (tt == TokenKind::Comma) {
        tokenStream.consumeKnownToken(tt);
        if (!tokenStream.getToken(&tt)) {
          return errorResult();
        }

        if (tt == TokenKind::LeftCurly) {
          if (!namedImports(importSpecSet)) {
            return errorResult();
          }
        } else if (tt == TokenKind::Mul) {
          if (!namespaceImport(importSpecSet)) {
            return errorResult();
          }
        } else {
          error(JSMSG_NAMED_IMPORTS_OR_NAMESPACE_IMPORT);
          return errorResult();
        }
      }
    } else {
      error(JSMSG_DECLARATION_AFTER_IMPORT);
      return errorResult();
    }

    if (!mustMatchToken(TokenKind::From, JSMSG_FROM_AFTER_IMPORT_CLAUSE)) {
      return errorResult();
    }

    if (!mustMatchToken(TokenKind::String, JSMSG_MODULE_SPEC_AFTER_FROM)) {
      return errorResult();
    }
  }

  NameNodeType moduleSpec;
  MOZ_TRY_VAR(moduleSpec, stringLiteral());

  // The `assert` keyword has a [no LineTerminator here] production before it in
  // the grammar -- `with` does not. We need to handle this distinction.
  if (!tokenStream.peekTokenSameLine(&tt, TokenStream::SlashIsRegExp)) {
    return errorResult();
  }

  // `with` may have an EOL prior, so peek the next token and replace
  // EOL if the next token is `with`.
  if (tt == TokenKind::Eol) {
    // Doing a regular peek won't produce Eol, but the actual next token.
    TokenKind peekedToken;
    if (!tokenStream.peekToken(&peekedToken, TokenStream::SlashIsRegExp)) {
      return errorResult();
    }

    if (peekedToken == TokenKind::With) {
      tt = TokenKind::With;
    }
  }

  ListNodeType importAttributeList;
  MOZ_TRY_VAR(importAttributeList,
              handler_.newList(ParseNodeKind::ImportAttributeList, pos()));

  if (tt == TokenKind::With ||
      (tt == TokenKind::Assert && options().importAttributesAssertSyntax())) {
    tokenStream.consumeKnownToken(tt, TokenStream::SlashIsRegExp);

    if (!withClause(importAttributeList)) {
      return errorResult();
    }
  }

  if (!matchOrInsertSemicolon(TokenStream::SlashIsRegExp)) {
    return errorResult();
  }

  BinaryNodeType moduleRequest;
  MOZ_TRY_VAR(moduleRequest,
              handler_.newModuleRequest(moduleSpec, importAttributeList,
                                        TokenPos(begin, pos().end)));

  BinaryNodeType node;
  MOZ_TRY_VAR(node, handler_.newImportDeclaration(importSpecSet, moduleRequest,
                                                  TokenPos(begin, pos().end)));
  if (!processImport(node)) {
    return errorResult();
  }

  return node;
}

template <class ParseHandler, typename Unit>
inline typename ParseHandler::NodeResult
GeneralParser<ParseHandler, Unit>::importDeclarationOrImportExpr(
    YieldHandling yieldHandling) {
  MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::Import));

  TokenKind tt;
  if (!tokenStream.peekToken(&tt)) {
    return errorResult();
  }

  if (tt == TokenKind::Dot || tt == TokenKind::LeftParen) {
    return expressionStatement(yieldHandling);
  }

  return importDeclaration();
}

template <typename Unit>
bool Parser<FullParseHandler, Unit>::checkExportedName(
    TaggedParserAtomIndex exportName) {
  if (!pc_->sc()->asModuleContext()->builder.hasExportedName(exportName)) {
    return true;
  }

  UniqueChars str = this->parserAtoms().toPrintableString(exportName);
  if (!str) {
    ReportOutOfMemory(this->fc_);
    return false;
  }

  error(JSMSG_DUPLICATE_EXPORT_NAME, str.get());
  return false;
}

template <typename Unit>
inline bool Parser<SyntaxParseHandler, Unit>::checkExportedName(
    TaggedParserAtomIndex exportName) {
  MOZ_ALWAYS_FALSE(abortIfSyntaxParser());
  return false;
}

template <class ParseHandler, typename Unit>
inline bool GeneralParser<ParseHandler, Unit>::checkExportedName(
    TaggedParserAtomIndex exportName) {
  return asFinalParser()->checkExportedName(exportName);
}

template <typename Unit>
bool Parser<FullParseHandler, Unit>::checkExportedNamesForArrayBinding(
    ListNode* array) {
  MOZ_ASSERT(array->isKind(ParseNodeKind::ArrayExpr));

  for (ParseNode* node : array->contents()) {
    if (node->isKind(ParseNodeKind::Elision)) {
      continue;
    }

    ParseNode* binding;
    if (node->isKind(ParseNodeKind::Spread)) {
      binding = node->as<UnaryNode>().kid();
    } else if (node->isKind(ParseNodeKind::AssignExpr)) {
      binding = node->as<AssignmentNode>().left();
    } else {
      binding = node;
    }

    if (!checkExportedNamesForDeclaration(binding)) {
      return false;
    }
  }

  return true;
}

template <typename Unit>
inline bool Parser<SyntaxParseHandler, Unit>::checkExportedNamesForArrayBinding(
    ListNodeType array) {
  MOZ_ALWAYS_FALSE(abortIfSyntaxParser());
  return false;
}

template <class ParseHandler, typename Unit>
inline bool
GeneralParser<ParseHandler, Unit>::checkExportedNamesForArrayBinding(
    ListNodeType array) {
  return asFinalParser()->checkExportedNamesForArrayBinding(array);
}

template <typename Unit>
bool Parser<FullParseHandler, Unit>::checkExportedNamesForObjectBinding(
    ListNode* obj) {
  MOZ_ASSERT(obj->isKind(ParseNodeKind::ObjectExpr));

  for (ParseNode* node : obj->contents()) {
    MOZ_ASSERT(node->isKind(ParseNodeKind::MutateProto) ||
               node->isKind(ParseNodeKind::PropertyDefinition) ||
               node->isKind(ParseNodeKind::Shorthand) ||
               node->isKind(ParseNodeKind::Spread));

    ParseNode* target;
    if (node->isKind(ParseNodeKind::Spread)) {
      target = node->as<UnaryNode>().kid();
    } else {
      if (node->isKind(ParseNodeKind::MutateProto)) {
        target = node->as<UnaryNode>().kid();
      } else {
        target = node->as<BinaryNode>().right();
      }

      if (target->isKind(ParseNodeKind::AssignExpr)) {
        target = target->as<AssignmentNode>().left();
      }
    }

    if (!checkExportedNamesForDeclaration(target)) {
      return false;
    }
  }

  return true;
}

template <typename Unit>
inline bool Parser<SyntaxParseHandler,
                   Unit>::checkExportedNamesForObjectBinding(ListNodeType obj) {
  MOZ_ALWAYS_FALSE(abortIfSyntaxParser());
  return false;
}

template <class ParseHandler, typename Unit>
inline bool
GeneralParser<ParseHandler, Unit>::checkExportedNamesForObjectBinding(
    ListNodeType obj) {
  return asFinalParser()->checkExportedNamesForObjectBinding(obj);
}

template <typename Unit>
bool Parser<FullParseHandler, Unit>::checkExportedNamesForDeclaration(
    ParseNode* node) {
  if (node->isKind(ParseNodeKind::Name)) {
    if (!checkExportedName(node->as<NameNode>().atom())) {
      return false;
    }
  } else if (node->isKind(ParseNodeKind::ArrayExpr)) {
    if (!checkExportedNamesForArrayBinding(&node->as<ListNode>())) {
      return false;
    }
  } else {
    MOZ_ASSERT(node->isKind(ParseNodeKind::ObjectExpr));
    if (!checkExportedNamesForObjectBinding(&node->as<ListNode>())) {
      return false;
    }
  }

  return true;
}

template <typename Unit>
inline bool Parser<SyntaxParseHandler, Unit>::checkExportedNamesForDeclaration(
    Node node) {
  MOZ_ALWAYS_FALSE(abortIfSyntaxParser());
  return false;
}

template <class ParseHandler, typename Unit>
inline bool GeneralParser<ParseHandler, Unit>::checkExportedNamesForDeclaration(
    Node node) {
  return asFinalParser()->checkExportedNamesForDeclaration(node);
}

template <typename Unit>
bool Parser<FullParseHandler, Unit>::checkExportedNamesForDeclarationList(
    DeclarationListNodeType node) {
  for (ParseNode* binding : node->contents()) {
    if (binding->isKind(ParseNodeKind::AssignExpr)) {
      binding = binding->as<AssignmentNode>().left();
    } else {
      MOZ_ASSERT(binding->isKind(ParseNodeKind::Name));
    }

    if (!checkExportedNamesForDeclaration(binding)) {
      return false;
    }
  }

  return true;
}

template <typename Unit>
inline bool
Parser<SyntaxParseHandler, Unit>::checkExportedNamesForDeclarationList(
    DeclarationListNodeType node) {
  MOZ_ALWAYS_FALSE(abortIfSyntaxParser());
  return false;
}

template <class ParseHandler, typename Unit>
inline bool
GeneralParser<ParseHandler, Unit>::checkExportedNamesForDeclarationList(
    DeclarationListNodeType node) {
  return asFinalParser()->checkExportedNamesForDeclarationList(node);
}

template <typename Unit>
inline bool Parser<FullParseHandler, Unit>::checkExportedNameForClause(
    NameNode* nameNode) {
  return checkExportedName(nameNode->atom());
}

template <typename Unit>
inline bool Parser<SyntaxParseHandler, Unit>::checkExportedNameForClause(
    NameNodeType nameNode) {
  MOZ_ALWAYS_FALSE(abortIfSyntaxParser());
  return false;
}

template <class ParseHandler, typename Unit>
inline bool GeneralParser<ParseHandler, Unit>::checkExportedNameForClause(
    NameNodeType nameNode) {
  return asFinalParser()->checkExportedNameForClause(nameNode);
}

template <typename Unit>
bool Parser<FullParseHandler, Unit>::checkExportedNameForFunction(
    FunctionNode* funNode) {
  return checkExportedName(funNode->funbox()->explicitName());
}

template <typename Unit>
inline bool Parser<SyntaxParseHandler, Unit>::checkExportedNameForFunction(
    FunctionNodeType funNode) {
  MOZ_ALWAYS_FALSE(abortIfSyntaxParser());
  return false;
}

template <class ParseHandler, typename Unit>
inline bool GeneralParser<ParseHandler, Unit>::checkExportedNameForFunction(
    FunctionNodeType funNode) {
  return asFinalParser()->checkExportedNameForFunction(funNode);
}

template <typename Unit>
bool Parser<FullParseHandler, Unit>::checkExportedNameForClass(
    ClassNode* classNode) {
  MOZ_ASSERT(classNode->names());
  return checkExportedName(classNode->names()->innerBinding()->atom());
}

template <typename Unit>
inline bool Parser<SyntaxParseHandler, Unit>::checkExportedNameForClass(
    ClassNodeType classNode) {
  MOZ_ALWAYS_FALSE(abortIfSyntaxParser());
  return false;
}

template <class ParseHandler, typename Unit>
inline bool GeneralParser<ParseHandler, Unit>::checkExportedNameForClass(
    ClassNodeType classNode) {
  return asFinalParser()->checkExportedNameForClass(classNode);
}

template <>
inline bool PerHandlerParser<FullParseHandler>::processExport(ParseNode* node) {
  return pc_->sc()->asModuleContext()->builder.processExport(node);
}

template <>
inline bool PerHandlerParser<SyntaxParseHandler>::processExport(Node node) {
  MOZ_ALWAYS_FALSE(abortIfSyntaxParser());
  return false;
}

template <>
inline bool PerHandlerParser<FullParseHandler>::processExportFrom(
    BinaryNodeType node) {
  return pc_->sc()->asModuleContext()->builder.processExportFrom(node);
}

template <>
inline bool PerHandlerParser<SyntaxParseHandler>::processExportFrom(
    BinaryNodeType node) {
  MOZ_ALWAYS_FALSE(abortIfSyntaxParser());
  return false;
}

template <>
inline bool PerHandlerParser<FullParseHandler>::processImport(
    BinaryNodeType node) {
  return pc_->sc()->asModuleContext()->builder.processImport(node);
}

template <>
inline bool PerHandlerParser<SyntaxParseHandler>::processImport(
    BinaryNodeType node) {
  MOZ_ALWAYS_FALSE(abortIfSyntaxParser());
  return false;
}

template <class ParseHandler, typename Unit>
typename ParseHandler::BinaryNodeResult
GeneralParser<ParseHandler, Unit>::exportFrom(uint32_t begin, Node specList) {
  if (!abortIfSyntaxParser()) {
    return errorResult();
  }

  MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::From));

  if (!mustMatchToken(TokenKind::String, JSMSG_MODULE_SPEC_AFTER_FROM)) {
    return errorResult();
  }

  NameNodeType moduleSpec;
  MOZ_TRY_VAR(moduleSpec, stringLiteral());

  TokenKind tt;

  // The `assert` keyword has a [no LineTerminator here] production before it in
  // the grammar -- `with` does not. We need to handle this distinction.
  if (!tokenStream.peekTokenSameLine(&tt, TokenStream::SlashIsRegExp)) {
    return errorResult();
  }

  // `with` may have an EOL prior, so peek the next token and replace
  // EOL if the next token is `with`.
  if (tt == TokenKind::Eol) {
    // Doing a regular peek won't produce Eol, but the actual next token.
    TokenKind peekedToken;
    if (!tokenStream.peekToken(&peekedToken, TokenStream::SlashIsRegExp)) {
      return errorResult();
    }

    if (peekedToken == TokenKind::With) {
      tt = TokenKind::With;
    }
  }

  uint32_t moduleSpecPos = pos().begin;

  ListNodeType importAttributeList;
  MOZ_TRY_VAR(importAttributeList,
              handler_.newList(ParseNodeKind::ImportAttributeList, pos()));
  if (tt == TokenKind::With ||
      (tt == TokenKind::Assert && options().importAttributesAssertSyntax())) {
    tokenStream.consumeKnownToken(tt, TokenStream::SlashIsRegExp);

    if (!withClause(importAttributeList)) {
      return errorResult();
    }
  }

  if (!matchOrInsertSemicolon(TokenStream::SlashIsRegExp)) {
    return errorResult();
  }

  BinaryNodeType moduleRequest;
  MOZ_TRY_VAR(moduleRequest,
              handler_.newModuleRequest(moduleSpec, importAttributeList,
                                        TokenPos(moduleSpecPos, pos().end)));

  BinaryNodeType node;
  MOZ_TRY_VAR(
      node, handler_.newExportFromDeclaration(begin, specList, moduleRequest));

  if (!processExportFrom(node)) {
    return errorResult();
  }

  return node;
}

template <class ParseHandler, typename Unit>
typename ParseHandler::BinaryNodeResult
GeneralParser<ParseHandler, Unit>::exportBatch(uint32_t begin) {
  if (!abortIfSyntaxParser()) {
    return errorResult();
  }

  MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::Mul));
  uint32_t beginExportSpec = pos().begin;

  ListNodeType kid;
  MOZ_TRY_VAR(kid, handler_.newList(ParseNodeKind::ExportSpecList, pos()));

  bool foundAs;
  if (!tokenStream.matchToken(&foundAs, TokenKind::As)) {
    return errorResult();
  }

  if (foundAs) {
    TokenKind tt;
    if (!tokenStream.getToken(&tt)) {
      return errorResult();
    }

    NameNodeType exportName = null();
    if (TokenKindIsPossibleIdentifierName(tt)) {
      MOZ_TRY_VAR(exportName, newName(anyChars.currentName()));
    } else if (tt == TokenKind::String) {
      MOZ_TRY_VAR(exportName, moduleExportName());
    } else {
      error(JSMSG_NO_EXPORT_NAME);
      return errorResult();
    }

    if (!checkExportedNameForClause(exportName)) {
      return errorResult();
    }

    UnaryNodeType exportSpec;
    MOZ_TRY_VAR(exportSpec,
                handler_.newExportNamespaceSpec(beginExportSpec, exportName));

    handler_.addList(kid, exportSpec);
  } else {
    // Handle the form |export *| by adding a special export batch
    // specifier to the list.
    NullaryNodeType exportSpec;
    MOZ_TRY_VAR(exportSpec, handler_.newExportBatchSpec(pos()));

    handler_.addList(kid, exportSpec);
  }

  if (!mustMatchToken(TokenKind::From, JSMSG_FROM_AFTER_EXPORT_STAR)) {
    return errorResult();
  }

  return exportFrom(begin, kid);
}

template <typename Unit>
bool Parser<FullParseHandler, Unit>::checkLocalExportNames(ListNode* node) {
  // ES 2017 draft 15.2.3.1.
  for (ParseNode* next : node->contents()) {
    ParseNode* name = next->as<BinaryNode>().left();

    if (name->isKind(ParseNodeKind::StringExpr)) {
      errorAt(name->pn_pos.begin, JSMSG_BAD_LOCAL_STRING_EXPORT);
      return false;
    }

    MOZ_ASSERT(name->isKind(ParseNodeKind::Name));

    TaggedParserAtomIndex ident = name->as<NameNode>().atom();
    if (!checkLocalExportName(ident, name->pn_pos.begin)) {
      return false;
    }
  }

  return true;
}

template <typename Unit>
bool Parser<SyntaxParseHandler, Unit>::checkLocalExportNames(
    ListNodeType node) {
  MOZ_ALWAYS_FALSE(abortIfSyntaxParser());
  return false;
}

template <class ParseHandler, typename Unit>
inline bool GeneralParser<ParseHandler, Unit>::checkLocalExportNames(
    ListNodeType node) {
  return asFinalParser()->checkLocalExportNames(node);
}

template <class ParseHandler, typename Unit>
typename ParseHandler::NodeResult
GeneralParser<ParseHandler, Unit>::exportClause(uint32_t begin) {
  if (!abortIfSyntaxParser()) {
    return errorResult();
  }

  MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::LeftCurly));

  ListNodeType kid;
  MOZ_TRY_VAR(kid, handler_.newList(ParseNodeKind::ExportSpecList, pos()));

  TokenKind tt;
  while (true) {
    // Handle the forms |export {}| and |export { ..., }| (where ... is non
    // empty), by escaping the loop early if the next token is }.
    if (!tokenStream.getToken(&tt)) {
      return errorResult();
    }

    if (tt == TokenKind::RightCurly) {
      break;
    }

    NameNodeType bindingName = null();
    if (TokenKindIsPossibleIdentifierName(tt)) {
      MOZ_TRY_VAR(bindingName, newName(anyChars.currentName()));
    } else if (tt == TokenKind::String) {
      MOZ_TRY_VAR(bindingName, moduleExportName());
    } else {
      error(JSMSG_NO_BINDING_NAME);
      return errorResult();
    }

    bool foundAs;
    if (!tokenStream.matchToken(&foundAs, TokenKind::As)) {
      return errorResult();
    }

    NameNodeType exportName = null();
    if (foundAs) {
      TokenKind tt;
      if (!tokenStream.getToken(&tt)) {
        return errorResult();
      }

      if (TokenKindIsPossibleIdentifierName(tt)) {
        MOZ_TRY_VAR(exportName, newName(anyChars.currentName()));
      } else if (tt == TokenKind::String) {
        MOZ_TRY_VAR(exportName, moduleExportName());
      } else {
        error(JSMSG_NO_EXPORT_NAME);
        return errorResult();
      }
    } else {
      if (tt != TokenKind::String) {
        MOZ_TRY_VAR(exportName, newName(anyChars.currentName()));
      } else {
        MOZ_TRY_VAR(exportName, moduleExportName());
      }
    }

    if (!checkExportedNameForClause(exportName)) {
      return errorResult();
    }

    BinaryNodeType exportSpec;
    MOZ_TRY_VAR(exportSpec, handler_.newExportSpec(bindingName, exportName));

    handler_.addList(kid, exportSpec);

    TokenKind next;
    if (!tokenStream.getToken(&next)) {
      return errorResult();
    }

    if (next == TokenKind::RightCurly) {
      break;
    }

    if (next != TokenKind::Comma) {
      error(JSMSG_RC_AFTER_EXPORT_SPEC_LIST);
      return errorResult();
    }
  }

  // Careful!  If |from| follows, even on a new line, it must start a
  // FromClause:
  //
  //   export { x }
  //   from "foo"; // a single ExportDeclaration
  //
  // But if it doesn't, we might have an ASI opportunity in SlashIsRegExp
  // context:
  //
  //   export { x }   // ExportDeclaration, terminated by ASI
  //   fro\u006D      // ExpressionStatement, the name "from"
  //
  // In that case let matchOrInsertSemicolon sort out ASI or any necessary
  // error.
  bool matched;
  if (!tokenStream.matchToken(&matched, TokenKind::From,
                              TokenStream::SlashIsRegExp)) {
    return errorResult();
  }

  if (matched) {
    return exportFrom(begin, kid);
  }

  if (!matchOrInsertSemicolon()) {
    return errorResult();
  }

  if (!checkLocalExportNames(kid)) {
    return errorResult();
  }

  UnaryNodeType node;
  MOZ_TRY_VAR(node,
              handler_.newExportDeclaration(kid, TokenPos(begin, pos().end)));

  if (!processExport(node)) {
    return errorResult();
  }

  return node;
}

template <class ParseHandler, typename Unit>
typename ParseHandler::UnaryNodeResult
GeneralParser<ParseHandler, Unit>::exportVariableStatement(uint32_t begin) {
  if (!abortIfSyntaxParser()) {
    return errorResult();
  }

  MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::Var));

  DeclarationListNodeType kid;
  MOZ_TRY_VAR(kid, declarationList(YieldIsName, ParseNodeKind::VarStmt));
  if (!matchOrInsertSemicolon()) {
    return errorResult();
  }
  if (!checkExportedNamesForDeclarationList(kid)) {
    return errorResult();
  }

  UnaryNodeType node;
  MOZ_TRY_VAR(node,
              handler_.newExportDeclaration(kid, TokenPos(begin, pos().end)));

  if (!processExport(node)) {
    return errorResult();
  }

  return node;
}

template <class ParseHandler, typename Unit>
typename ParseHandler::UnaryNodeResult
GeneralParser<ParseHandler, Unit>::exportFunctionDeclaration(
    uint32_t begin, uint32_t toStringStart,
    FunctionAsyncKind asyncKind /* = SyncFunction */) {
  if (!abortIfSyntaxParser()) {
    return errorResult();
  }

  MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::Function));

  Node kid;
  MOZ_TRY_VAR(
      kid, functionStmt(toStringStart, YieldIsName, NameRequired, asyncKind));

  if (!checkExportedNameForFunction(handler_.asFunctionNode(kid))) {
    return errorResult();
  }

  UnaryNodeType node;
  MOZ_TRY_VAR(node,
              handler_.newExportDeclaration(kid, TokenPos(begin, pos().end)));

  if (!processExport(node)) {
    return errorResult();
  }

  return node;
}

template <class ParseHandler, typename Unit>
typename ParseHandler::UnaryNodeResult
GeneralParser<ParseHandler, Unit>::exportClassDeclaration(uint32_t begin) {
  if (!abortIfSyntaxParser()) {
    return errorResult();
  }

  MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::Class));

  ClassNodeType kid;
  MOZ_TRY_VAR(kid, classDefinition(YieldIsName, ClassStatement, NameRequired));

  if (!checkExportedNameForClass(kid)) {
    return errorResult();
  }

  UnaryNodeType node;
  MOZ_TRY_VAR(node,
              handler_.newExportDeclaration(kid, TokenPos(begin, pos().end)));

  if (!processExport(node)) {
    return errorResult();
  }

  return node;
}

template <class ParseHandler, typename Unit>
typename ParseHandler::UnaryNodeResult
GeneralParser<ParseHandler, Unit>::exportLexicalDeclaration(
    uint32_t begin, DeclarationKind kind) {
  if (!abortIfSyntaxParser()) {
    return errorResult();
  }

  MOZ_ASSERT(kind == DeclarationKind::Const || kind == DeclarationKind::Let);
  MOZ_ASSERT_IF(kind == DeclarationKind::Const,
                anyChars.isCurrentTokenType(TokenKind::Const));
  MOZ_ASSERT_IF(kind == DeclarationKind::Let,
                anyChars.isCurrentTokenType(TokenKind::Let));

  DeclarationListNodeType kid;
  MOZ_TRY_VAR(kid, lexicalDeclaration(YieldIsName, kind));
  if (!checkExportedNamesForDeclarationList(kid)) {
    return errorResult();
  }

  UnaryNodeType node;
  MOZ_TRY_VAR(node,
              handler_.newExportDeclaration(kid, TokenPos(begin, pos().end)));

  if (!processExport(node)) {
    return errorResult();
  }

  return node;
}

template <class ParseHandler, typename Unit>
typename ParseHandler::BinaryNodeResult
GeneralParser<ParseHandler, Unit>::exportDefaultFunctionDeclaration(
    uint32_t begin, uint32_t toStringStart,
    FunctionAsyncKind asyncKind /* = SyncFunction */) {
  if (!abortIfSyntaxParser()) {
    return errorResult();
  }

  MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::Function));

  Node kid;
  MOZ_TRY_VAR(kid, functionStmt(toStringStart, YieldIsName, AllowDefaultName,
                                asyncKind));

  BinaryNodeType node;
  MOZ_TRY_VAR(node, handler_.newExportDefaultDeclaration(
                        kid, null(), TokenPos(begin, pos().end)));

  if (!processExport(node)) {
    return errorResult();
  }

  return node;
}

template <class ParseHandler, typename Unit>
typename ParseHandler::BinaryNodeResult
GeneralParser<ParseHandler, Unit>::exportDefaultClassDeclaration(
    uint32_t begin) {
  if (!abortIfSyntaxParser()) {
    return errorResult();
  }

  MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::Class));

  ClassNodeType kid;
  MOZ_TRY_VAR(kid,
              classDefinition(YieldIsName, ClassStatement, AllowDefaultName));

  BinaryNodeType node;
  MOZ_TRY_VAR(node, handler_.newExportDefaultDeclaration(
                        kid, null(), TokenPos(begin, pos().end)));

  if (!processExport(node)) {
    return errorResult();
  }

  return node;
}

template <class ParseHandler, typename Unit>
typename ParseHandler::BinaryNodeResult
GeneralParser<ParseHandler, Unit>::exportDefaultAssignExpr(uint32_t begin) {
  if (!abortIfSyntaxParser()) {
    return errorResult();
  }

  TaggedParserAtomIndex name = TaggedParserAtomIndex::WellKnown::default_();
  NameNodeType nameNode;
  MOZ_TRY_VAR(nameNode, newName(name));
  if (!noteDeclaredName(name, DeclarationKind::Const, pos())) {
    return errorResult();
  }

  Node kid;
  MOZ_TRY_VAR(kid, assignExpr(InAllowed, YieldIsName, TripledotProhibited));

  if (!matchOrInsertSemicolon()) {
    return errorResult();
  }

  BinaryNodeType node;
  MOZ_TRY_VAR(node, handler_.newExportDefaultDeclaration(
                        kid, nameNode, TokenPos(begin, pos().end)));

  if (!processExport(node)) {
    return errorResult();
  }

  return node;
}

template <class ParseHandler, typename Unit>
typename ParseHandler::BinaryNodeResult
GeneralParser<ParseHandler, Unit>::exportDefault(uint32_t begin) {
  if (!abortIfSyntaxParser()) {
    return errorResult();
  }

  MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::Default));

  TokenKind tt;
  if (!tokenStream.getToken(&tt, TokenStream::SlashIsRegExp)) {
    return errorResult();
  }

  if (!checkExportedName(TaggedParserAtomIndex::WellKnown::default_())) {
    return errorResult();
  }

  switch (tt) {
    case TokenKind::Function:
      return exportDefaultFunctionDeclaration(begin, pos().begin);

    case TokenKind::Async: {
      TokenKind nextSameLine = TokenKind::Eof;
      if (!tokenStream.peekTokenSameLine(&nextSameLine)) {
        return errorResult();
      }

      if (nextSameLine == TokenKind::Function) {
        uint32_t toStringStart = pos().begin;
        tokenStream.consumeKnownToken(TokenKind::Function);
        return exportDefaultFunctionDeclaration(
            begin, toStringStart, FunctionAsyncKind::AsyncFunction);
      }

      anyChars.ungetToken();
      return exportDefaultAssignExpr(begin);
    }

    case TokenKind::Class:
      return exportDefaultClassDeclaration(begin);

    default:
      anyChars.ungetToken();
      return exportDefaultAssignExpr(begin);
  }
}

template <class ParseHandler, typename Unit>
typename ParseHandler::NodeResult
GeneralParser<ParseHandler, Unit>::exportDeclaration() {
  if (!abortIfSyntaxParser()) {
    return errorResult();
  }

  MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::Export));

  if (!pc_->atModuleLevel()) {
    error(JSMSG_EXPORT_DECL_AT_TOP_LEVEL);
    return errorResult();
  }

  uint32_t begin = pos().begin;

  TokenKind tt;
  if (!tokenStream.getToken(&tt)) {
    return errorResult();
  }
  switch (tt) {
    case TokenKind::Mul:
      return exportBatch(begin);

    case TokenKind::LeftCurly:
      return exportClause(begin);

    case TokenKind::Var:
      return exportVariableStatement(begin);

    case TokenKind::Function:
      return exportFunctionDeclaration(begin, pos().begin);

    case TokenKind::Async: {
      TokenKind nextSameLine = TokenKind::Eof;
      if (!tokenStream.peekTokenSameLine(&nextSameLine)) {
        return errorResult();
      }

      if (nextSameLine == TokenKind::Function) {
        uint32_t toStringStart = pos().begin;
        tokenStream.consumeKnownToken(TokenKind::Function);
        return exportFunctionDeclaration(begin, toStringStart,
                                         FunctionAsyncKind::AsyncFunction);
      }

      error(JSMSG_DECLARATION_AFTER_EXPORT);
      return errorResult();
    }

    case TokenKind::Class:
      return exportClassDeclaration(begin);

    case TokenKind::Const:
      return exportLexicalDeclaration(begin, DeclarationKind::Const);

    case TokenKind::Let:
      return exportLexicalDeclaration(begin, DeclarationKind::Let);

    case TokenKind::Default:
      return exportDefault(begin);

    default:
      error(JSMSG_DECLARATION_AFTER_EXPORT);
      return errorResult();
  }
}

template <class ParseHandler, typename Unit>
typename ParseHandler::UnaryNodeResult
GeneralParser<ParseHandler, Unit>::expressionStatement(
    YieldHandling yieldHandling, InvokedPrediction invoked) {
  anyChars.ungetToken();
  Node pnexpr;
  MOZ_TRY_VAR(pnexpr, expr(InAllowed, yieldHandling, TripledotProhibited,
                           /* possibleError = */ nullptr, invoked));
  if (!matchOrInsertSemicolon()) {
    return errorResult();
  }
  return handler_.newExprStatement(pnexpr, pos().end);
}

template <class ParseHandler, typename Unit>
typename ParseHandler::NodeResult
GeneralParser<ParseHandler, Unit>::consequentOrAlternative(
    YieldHandling yieldHandling) {
  TokenKind next;
  if (!tokenStream.peekToken(&next, TokenStream::SlashIsRegExp)) {
    return errorResult();
  }

  // Annex B.3.4 says that unbraced FunctionDeclarations under if/else in
  // non-strict code act as if they were braced: |if (x) function f() {}|
  // parses as |if (x) { function f() {} }|.
  //
  // Careful!  FunctionDeclaration doesn't include generators or async
  // functions.
  if (next == TokenKind::Function) {
    tokenStream.consumeKnownToken(next, TokenStream::SlashIsRegExp);

    // Parser::statement would handle this, but as this function handles
    // every other error case, it seems best to handle this.
    if (pc_->sc()->strict()) {
      error(JSMSG_FORBIDDEN_AS_STATEMENT, "function declarations");
      return errorResult();
    }

    TokenKind maybeStar;
    if (!tokenStream.peekToken(&maybeStar)) {
      return errorResult();
    }

    if (maybeStar == TokenKind::Mul) {
      error(JSMSG_FORBIDDEN_AS_STATEMENT, "generator declarations");
      return errorResult();
    }

    ParseContext::Statement stmt(pc_, StatementKind::Block);
    ParseContext::Scope scope(this);
    if (!scope.init(pc_)) {
      return errorResult();
    }

    TokenPos funcPos = pos();
    Node fun;
    MOZ_TRY_VAR(fun, functionStmt(pos().begin, yieldHandling, NameRequired));

    ListNodeType block;
    MOZ_TRY_VAR(block, handler_.newStatementList(funcPos));

    handler_.addStatementToList(block, fun);
    return finishLexicalScope(scope, block);
  }

  return statement(yieldHandling);
}

template <class ParseHandler, typename Unit>
typename ParseHandler::TernaryNodeResult
GeneralParser<ParseHandler, Unit>::ifStatement(YieldHandling yieldHandling) {
  Vector<Node, 4> condList(fc_), thenList(fc_);
  Vector<uint32_t, 4> posList(fc_);
  Node elseBranch;

  ParseContext::Statement stmt(pc_, StatementKind::If);

  while (true) {
    uint32_t begin = pos().begin;

    /* An IF node has three kids: condition, then, and optional else. */
    Node cond;
    MOZ_TRY_VAR(cond, condition(InAllowed, yieldHandling));

    TokenKind tt;
    if (!tokenStream.peekToken(&tt, TokenStream::SlashIsRegExp)) {
      return errorResult();
    }

    Node thenBranch;
    MOZ_TRY_VAR(thenBranch, consequentOrAlternative(yieldHandling));

    if (!condList.append(cond) || !thenList.append(thenBranch) ||
        !posList.append(begin)) {
      return errorResult();
    }

    bool matched;
    if (!tokenStream.matchToken(&matched, TokenKind::Else,
                                TokenStream::SlashIsRegExp)) {
      return errorResult();
    }
    if (matched) {
      if (!tokenStream.matchToken(&matched, TokenKind::If,
                                  TokenStream::SlashIsRegExp)) {
        return errorResult();
      }
      if (matched) {
        continue;
      }
      MOZ_TRY_VAR(elseBranch, consequentOrAlternative(yieldHandling));
    } else {
      elseBranch = null();
    }
    break;
  }

  TernaryNodeType ifNode;
  for (int i = condList.length() - 1; i >= 0; i--) {
    MOZ_TRY_VAR(ifNode, handler_.newIfStatement(posList[i], condList[i],
                                                thenList[i], elseBranch));
    elseBranch = ifNode;
  }

  return ifNode;
}

template <class ParseHandler, typename Unit>
typename ParseHandler::BinaryNodeResult
GeneralParser<ParseHandler, Unit>::doWhileStatement(
    YieldHandling yieldHandling) {
  uint32_t begin = pos().begin;
  ParseContext::Statement stmt(pc_, StatementKind::DoLoop);
  Node body;
  MOZ_TRY_VAR(body, statement(yieldHandling));
  if (!mustMatchToken(TokenKind::While, JSMSG_WHILE_AFTER_DO)) {
    return errorResult();
  }
  Node cond;
  MOZ_TRY_VAR(cond, condition(InAllowed, yieldHandling));

  // The semicolon after do-while is even more optional than most
  // semicolons in JS.  Web compat required this by 2004:
  //   http://bugzilla.mozilla.org/show_bug.cgi?id=238945
  // ES3 and ES5 disagreed, but ES6 conforms to Web reality:
  //   https://bugs.ecmascript.org/show_bug.cgi?id=157
  // To parse |do {} while (true) false| correctly, use SlashIsRegExp.
  bool ignored;
  if (!tokenStream.matchToken(&ignored, TokenKind::Semi,
                              TokenStream::SlashIsRegExp)) {
    return errorResult();
  }
  return handler_.newDoWhileStatement(body, cond, TokenPos(begin, pos().end));
}

template <class ParseHandler, typename Unit>
typename ParseHandler::BinaryNodeResult
GeneralParser<ParseHandler, Unit>::whileStatement(YieldHandling yieldHandling) {
  uint32_t begin = pos().begin;
  ParseContext::Statement stmt(pc_, StatementKind::WhileLoop);
  Node cond;
  MOZ_TRY_VAR(cond, condition(InAllowed, yieldHandling));
  Node body;
  MOZ_TRY_VAR(body, statement(yieldHandling));
  return handler_.newWhileStatement(begin, cond, body);
}

template <class ParseHandler, typename Unit>
bool GeneralParser<ParseHandler, Unit>::matchInOrOf(bool* isForInp,
                                                    bool* isForOfp) {
  TokenKind tt;
  if (!tokenStream.getToken(&tt, TokenStream::SlashIsRegExp)) {
    return false;
  }

  *isForInp = tt == TokenKind::In;
  *isForOfp = tt == TokenKind::Of;
  if (!*isForInp && !*isForOfp) {
    anyChars.ungetToken();
  }

  MOZ_ASSERT_IF(*isForInp || *isForOfp, *isForInp != *isForOfp);
  return true;
}

template <class ParseHandler, typename Unit>
bool GeneralParser<ParseHandler, Unit>::forHeadStart(
    YieldHandling yieldHandling, IteratorKind iterKind,
    ParseNodeKind* forHeadKind, Node* forInitialPart,
    Maybe<ParseContext::Scope>& forLoopLexicalScope,
    Node* forInOrOfExpression) {
  MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::LeftParen));

  TokenKind tt;
  if (!tokenStream.peekToken(&tt, TokenStream::SlashIsRegExp)) {
    return false;
  }

  // Super-duper easy case: |for (;| is a C-style for-loop with no init
  // component.
  if (tt == TokenKind::Semi) {
    *forInitialPart = null();
    *forHeadKind = ParseNodeKind::ForHead;
    return true;
  }

  // Parsing after |for (var| is also relatively simple (from this method's
  // point of view).  No block-related work complicates matters, so delegate
  // to Parser::declaration.
  if (tt == TokenKind::Var) {
    tokenStream.consumeKnownToken(tt, TokenStream::SlashIsRegExp);

    // Pass null for block object because |var| declarations don't use one.
    MOZ_TRY_VAR_OR_RETURN(*forInitialPart,
                          declarationList(yieldHandling, ParseNodeKind::VarStmt,
                                          forHeadKind, forInOrOfExpression),
                          false);
    return true;
  }

  // Otherwise we have a lexical declaration or an expression.

  // For-in loop backwards compatibility requires that |let| starting a
  // for-loop that's not a (new to ES6) for-of loop, in non-strict mode code,
  // parse as an identifier.  (|let| in for-of is always a declaration.)
  //
  // For-of loops can't start with the token sequence "async of", because that
  // leads to a shift-reduce conflict when parsing |for (async of => {};;)| or
  // |for (async of [])|.
  bool parsingLexicalDeclaration = false;
  bool letIsIdentifier = false;
  bool startsWithForOf = false;

  if (tt == TokenKind::Const) {
    parsingLexicalDeclaration = true;
    tokenStream.consumeKnownToken(tt, TokenStream::SlashIsRegExp);
  }
#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
  else if (tt == TokenKind::Await) {
    if (!pc_->isAsync()) {
      if (pc_->atModuleTopLevel()) {
        if (!options().topLevelAwait) {
          error(JSMSG_TOP_LEVEL_AWAIT_NOT_SUPPORTED);
          return false;
        }
        pc_->sc()->asModuleContext()->setIsAsync();
        MOZ_ASSERT(pc_->isAsync());
      }
    }
    if (pc_->isAsync()) {
      // Try finding evidence of a AwaitUsingDeclaration the syntax for which
      // would be:
      //   await [no LineTerminator here] using [no LineTerminator here]
      //     identifier
      tokenStream.consumeKnownToken(tt, TokenStream::SlashIsRegExp);

      TokenKind nextTok = TokenKind::Eof;
      if (!tokenStream.peekTokenSameLine(&nextTok,
                                         TokenStream::SlashIsRegExp)) {
        return false;
      }

      if (nextTok == TokenKind::Using) {
        tokenStream.consumeKnownToken(nextTok, TokenStream::SlashIsRegExp);

        TokenKind nextTokIdent = TokenKind::Eof;
        if (!tokenStream.peekTokenSameLine(&nextTokIdent)) {
          return false;
        }

        if (TokenKindIsPossibleIdentifier(nextTokIdent)) {
          parsingLexicalDeclaration = true;
        } else {
          anyChars.ungetToken();  // put back using token
          anyChars.ungetToken();  // put back await token
        }
      } else {
        anyChars.ungetToken();  // put back await token
      }
    }
  } else if (tt == TokenKind::Using) {
    tokenStream.consumeKnownToken(tt, TokenStream::SlashIsRegExp);

    // Look ahead to find either a 'of' token or if not identifier
    TokenKind nextTok = TokenKind::Eof;
    if (!tokenStream.peekTokenSameLine(&nextTok)) {
      return false;
    }

    if (nextTok == TokenKind::Of || !TokenKindIsPossibleIdentifier(nextTok)) {
      anyChars.ungetToken();  // we didnt find a valid case of using decl put
                              // back the token
    } else {
      parsingLexicalDeclaration = true;
    }
  }
#endif
  else if (tt == TokenKind::Let) {
    // We could have a {For,Lexical}Declaration, or we could have a
    // LeftHandSideExpression with lookahead restrictions so it's not
    // ambiguous with the former.  Check for a continuation of the former
    // to decide which we have.
    tokenStream.consumeKnownToken(TokenKind::Let, TokenStream::SlashIsRegExp);

    TokenKind next;
    if (!tokenStream.peekToken(&next)) {
      return false;
    }

    parsingLexicalDeclaration = nextTokenContinuesLetDeclaration(next);
    if (!parsingLexicalDeclaration) {
      // If we end up here, we may have `for (let <reserved word> of/in ...`,
      // which is not valid.
      if (next != TokenKind::In && next != TokenKind::Of &&
          TokenKindIsReservedWord(next)) {
        tokenStream.consumeKnownToken(next);
        error(JSMSG_UNEXPECTED_TOKEN_NO_EXPECT, TokenKindToDesc(next));
        return false;
      }

      anyChars.ungetToken();
      letIsIdentifier = true;
    }
  } else if (tt == TokenKind::Async && iterKind == IteratorKind::Sync) {
    tokenStream.consumeKnownToken(TokenKind::Async, TokenStream::SlashIsRegExp);

    TokenKind next;
    if (!tokenStream.peekToken(&next)) {
      return false;
    }

    if (next == TokenKind::Of) {
      startsWithForOf = true;
    }
    anyChars.ungetToken();
  }

  if (parsingLexicalDeclaration) {
    if (options().selfHostingMode) {
      error(JSMSG_SELFHOSTED_LEXICAL);
      return false;
    }

    forLoopLexicalScope.emplace(this);
    if (!forLoopLexicalScope->init(pc_)) {
      return false;
    }

    // Push a temporary ForLoopLexicalHead Statement that allows for
    // lexical declarations, as they are usually allowed only in braced
    // statements.
    ParseContext::Statement forHeadStmt(pc_, StatementKind::ForLoopLexicalHead);

    ParseNodeKind declKind;
    switch (tt) {
      case TokenKind::Const:
        declKind = ParseNodeKind::ConstDecl;
        break;
#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
      case TokenKind::Using:
        declKind = ParseNodeKind::UsingDecl;
        break;
      case TokenKind::Await:
        declKind = ParseNodeKind::AwaitUsingDecl;
        break;
#endif
      case TokenKind::Let:
        declKind = ParseNodeKind::LetDecl;
        break;
      default:
        MOZ_CRASH("unexpected node kind");
    }

    MOZ_TRY_VAR_OR_RETURN(*forInitialPart,
                          declarationList(yieldHandling, declKind, forHeadKind,
                                          forInOrOfExpression),
                          false);
    return true;
  }

  uint32_t exprOffset;
  if (!tokenStream.peekOffset(&exprOffset, TokenStream::SlashIsRegExp)) {
    return false;
  }

  // Finally, handle for-loops that start with expressions.  Pass
  // |InProhibited| so that |in| isn't parsed in a RelationalExpression as a
  // binary operator.  |in| makes it a for-in loop, *not* an |in| expression.
  PossibleError possibleError(*this);
  MOZ_TRY_VAR_OR_RETURN(
      *forInitialPart,
      expr(InProhibited, yieldHandling, TripledotProhibited, &possibleError),
      false);

  bool isForIn, isForOf;
  if (!matchInOrOf(&isForIn, &isForOf)) {
    return false;
  }

  // If we don't encounter 'in'/'of', we have a for(;;) loop.  We've handled
  // the init expression; the caller handles the rest.
  if (!isForIn && !isForOf) {
    if (!possibleError.checkForExpressionError()) {
      return false;
    }

    *forHeadKind = ParseNodeKind::ForHead;
    return true;
  }

  MOZ_ASSERT(isForIn != isForOf);

  // In a for-of loop, 'let' that starts the loop head is a |let| keyword,
  // per the [lookahead ≠ let] restriction on the LeftHandSideExpression
  // variant of such loops.  Expressions that start with |let| can't be used
  // here.
  //
  //   var let = {};
  //   for (let.prop of [1]) // BAD
  //     break;
  //
  // See ES6 13.7.
  if (isForOf && letIsIdentifier) {
    errorAt(exprOffset, JSMSG_BAD_STARTING_FOROF_LHS, "let");
    return false;
  }

  // In a for-of loop, the LeftHandSideExpression isn't allowed to be an
  // identifier named "async" per the [lookahead ≠ async of] restriction.
  if (isForOf && startsWithForOf) {
    errorAt(exprOffset, JSMSG_BAD_STARTING_FOROF_LHS, "async of");
    return false;
  }

  *forHeadKind = isForIn ? ParseNodeKind::ForIn : ParseNodeKind::ForOf;

  // Verify the left-hand side expression doesn't have a forbidden form.
  if (handler_.isUnparenthesizedDestructuringPattern(*forInitialPart)) {
    if (!possibleError.checkForDestructuringErrorOrWarning()) {
      return false;
    }
  } else if (handler_.isName(*forInitialPart)) {
    if (const char* chars = nameIsArgumentsOrEval(*forInitialPart)) {
      // |chars| is "arguments" or "eval" here.
      if (!strictModeErrorAt(exprOffset, JSMSG_BAD_STRICT_ASSIGN, chars)) {
        return false;
      }
    }
  } else if (handler_.isArgumentsLength(*forInitialPart)) {
    pc_->sc()->setIneligibleForArgumentsLength();
  } else if (handler_.isPropertyOrPrivateMemberAccess(*forInitialPart)) {
    // Permitted: no additional testing/fixup needed.
  } else if (handler_.isFunctionCall(*forInitialPart)) {
    if (!strictModeErrorAt(exprOffset, JSMSG_BAD_FOR_LEFTSIDE)) {
      return false;
    }
  } else {
    errorAt(exprOffset, JSMSG_BAD_FOR_LEFTSIDE);
    return false;
  }

  if (!possibleError.checkForExpressionError()) {
    return false;
  }

  // Finally, parse the iterated expression, making the for-loop's closing
  // ')' the next token.
  MOZ_TRY_VAR_OR_RETURN(*forInOrOfExpression,
                        expressionAfterForInOrOf(*forHeadKind, yieldHandling),
                        false);
  return true;
}

template <class ParseHandler, typename Unit>
typename ParseHandler::NodeResult
GeneralParser<ParseHandler, Unit>::forStatement(YieldHandling yieldHandling) {
  MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::For));

  uint32_t begin = pos().begin;

  ParseContext::Statement stmt(pc_, StatementKind::ForLoop);

  IteratorKind iterKind = IteratorKind::Sync;
  unsigned iflags = 0;

  if (pc_->isAsync() || pc_->sc()->isModuleContext()) {
    bool matched;
    if (!tokenStream.matchToken(&matched, TokenKind::Await)) {
      return errorResult();
    }

    // If we come across a top level await here, mark the module as async.
    if (matched && pc_->sc()->isModuleContext() && !pc_->isAsync()) {
      if (!options().topLevelAwait) {
        error(JSMSG_TOP_LEVEL_AWAIT_NOT_SUPPORTED);
        return errorResult();
      }
      pc_->sc()->asModuleContext()->setIsAsync();
      MOZ_ASSERT(pc_->isAsync());
    }

    if (matched) {
      iflags |= JSITER_FORAWAITOF;
      iterKind = IteratorKind::Async;
    }
  }

  if (!mustMatchToken(TokenKind::LeftParen, [this](TokenKind actual) {
        this->error((actual == TokenKind::Await && !this->pc_->isAsync())
                        ? JSMSG_FOR_AWAIT_OUTSIDE_ASYNC
                        : JSMSG_PAREN_AFTER_FOR);
      })) {
    return errorResult();
  }

  // ParseNodeKind::ForHead, ParseNodeKind::ForIn, or
  // ParseNodeKind::ForOf depending on the loop type.
  ParseNodeKind headKind;

  // |x| in either |for (x; ...; ...)| or |for (x in/of ...)|.
  Node startNode;

  // The next two variables are used to implement `for (let/const ...)`.
  //
  // We generate an implicit block, wrapping the whole loop, to store loop
  // variables declared this way. Note that if the loop uses `for (var...)`
  // instead, those variables go on some existing enclosing scope, so no
  // implicit block scope is created.
  //
  // Both variables remain null/none if the loop is any other form.

  // The static block scope for the implicit block scope.
  Maybe<ParseContext::Scope> forLoopLexicalScope;

  // The expression being iterated over, for for-in/of loops only.  Unused
  // for for(;;) loops.
  Node iteratedExpr;

  // Parse the entirety of the loop-head for a for-in/of loop (so the next
  // token is the closing ')'):
  //
  //   for (... in/of ...) ...
  //                     ^next token
  //
  // ...OR, parse up to the first ';' in a C-style for-loop:
  //
  //   for (...; ...; ...) ...
  //           ^next token
  //
  // In either case the subsequent token can be consistently accessed using
  // TokenStream::SlashIsDiv semantics.
  if (!forHeadStart(yieldHandling, iterKind, &headKind, &startNode,
                    forLoopLexicalScope, &iteratedExpr)) {
    return errorResult();
  }

  MOZ_ASSERT(headKind == ParseNodeKind::ForIn ||
             headKind == ParseNodeKind::ForOf ||
             headKind == ParseNodeKind::ForHead);

  if (iterKind == IteratorKind::Async && headKind != ParseNodeKind::ForOf) {
    errorAt(begin, JSMSG_FOR_AWAIT_NOT_OF);
    return errorResult();
  }

  TernaryNodeType forHead;
  if (headKind == ParseNodeKind::ForHead) {
    Node init = startNode;

    // Look for an operand: |for (;| means we might have already examined
    // this semicolon with that modifier.
    if (!mustMatchToken(TokenKind::Semi, JSMSG_SEMI_AFTER_FOR_INIT)) {
      return errorResult();
    }

    TokenKind tt;
    if (!tokenStream.peekToken(&tt, TokenStream::SlashIsRegExp)) {
      return errorResult();
    }

    Node test;
    if (tt == TokenKind::Semi) {
      test = null();
    } else {
      MOZ_TRY_VAR(test, expr(InAllowed, yieldHandling, TripledotProhibited));
    }

    if (!mustMatchToken(TokenKind::Semi, JSMSG_SEMI_AFTER_FOR_COND)) {
      return errorResult();
    }

    if (!tokenStream.peekToken(&tt, TokenStream::SlashIsRegExp)) {
      return errorResult();
    }

    Node update;
    if (tt == TokenKind::RightParen) {
      update = null();
    } else {
      MOZ_TRY_VAR(update, expr(InAllowed, yieldHandling, TripledotProhibited));
    }

    if (!mustMatchToken(TokenKind::RightParen, JSMSG_PAREN_AFTER_FOR_CTRL)) {
      return errorResult();
    }

    TokenPos headPos(begin, pos().end);
    MOZ_TRY_VAR(forHead, handler_.newForHead(init, test, update, headPos));
  } else {
    MOZ_ASSERT(headKind == ParseNodeKind::ForIn ||
               headKind == ParseNodeKind::ForOf);

    // |target| is the LeftHandSideExpression or declaration to which the
    // per-iteration value (an arbitrary value exposed by the iteration
    // protocol, or a string naming a property) is assigned.
    Node target = startNode;

    // Parse the rest of the for-in/of head.
    if (headKind == ParseNodeKind::ForIn) {
      stmt.refineForKind(StatementKind::ForInLoop);
    } else {
      stmt.refineForKind(StatementKind::ForOfLoop);
    }

    // Parser::declaration consumed everything up to the closing ')'.  That
    // token follows an {Assignment,}Expression and so must be interpreted
    // as an operand to be consistent with normal expression tokenizing.
    if (!mustMatchToken(TokenKind::RightParen, JSMSG_PAREN_AFTER_FOR_CTRL)) {
      return errorResult();
    }

    TokenPos headPos(begin, pos().end);
    MOZ_TRY_VAR(forHead, handler_.newForInOrOfHead(headKind, target,
                                                   iteratedExpr, headPos));
  }

  Node body;
  MOZ_TRY_VAR(body, statement(yieldHandling));

  ForNodeType forLoop;
  MOZ_TRY_VAR(forLoop, handler_.newForStatement(begin, forHead, body, iflags));

  if (forLoopLexicalScope) {
    return finishLexicalScope(*forLoopLexicalScope, forLoop);
  }

  return forLoop;
}

template <class ParseHandler, typename Unit>
typename ParseHandler::SwitchStatementResult
GeneralParser<ParseHandler, Unit>::switchStatement(
    YieldHandling yieldHandling) {
  MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::Switch));
  uint32_t begin = pos().begin;

  if (!mustMatchToken(TokenKind::LeftParen, JSMSG_PAREN_BEFORE_SWITCH)) {
    return errorResult();
  }

  Node discriminant;
  MOZ_TRY_VAR(discriminant,
              exprInParens(InAllowed, yieldHandling, TripledotProhibited));

  if (!mustMatchToken(TokenKind::RightParen, JSMSG_PAREN_AFTER_SWITCH)) {
    return errorResult();
  }
  if (!mustMatchToken(TokenKind::LeftCurly, JSMSG_CURLY_BEFORE_SWITCH)) {
    return errorResult();
  }

  ParseContext::Statement stmt(pc_, StatementKind::Switch);
  ParseContext::Scope scope(this);
  if (!scope.init(pc_)) {
    return errorResult();
  }

  ListNodeType caseList;
  MOZ_TRY_VAR(caseList, handler_.newStatementList(pos()));

  bool seenDefault = false;
  TokenKind tt;
  while (true) {
    if (!tokenStream.getToken(&tt, TokenStream::SlashIsRegExp)) {
      return errorResult();
    }
    if (tt == TokenKind::RightCurly) {
      break;
    }
    uint32_t caseBegin = pos().begin;

    Node caseExpr;
    switch (tt) {
      case TokenKind::Default:
        if (seenDefault) {
          error(JSMSG_TOO_MANY_DEFAULTS);
          return errorResult();
        }
        seenDefault = true;
        caseExpr = null();  // The default case has pn_left == nullptr.
        break;

      case TokenKind::Case:
        MOZ_TRY_VAR(caseExpr,
                    expr(InAllowed, yieldHandling, TripledotProhibited));
        break;

      default:
        error(JSMSG_BAD_SWITCH);
        return errorResult();
    }

    if (!mustMatchToken(TokenKind::Colon, JSMSG_COLON_AFTER_CASE)) {
      return errorResult();
    }

    ListNodeType body;
    MOZ_TRY_VAR(body, handler_.newStatementList(pos()));

    bool afterReturn = false;
    bool warnedAboutStatementsAfterReturn = false;
    uint32_t statementBegin = 0;
    while (true) {
      if (!tokenStream.peekToken(&tt, TokenStream::SlashIsRegExp)) {
        return errorResult();
      }
      if (tt == TokenKind::RightCurly || tt == TokenKind::Case ||
          tt == TokenKind::Default) {
        break;
      }
      if (afterReturn) {
        if (!tokenStream.peekOffset(&statementBegin,
                                    TokenStream::SlashIsRegExp)) {
          return errorResult();
        }
      }
      Node stmt;
      MOZ_TRY_VAR(stmt, statementListItem(yieldHandling));
      if (!warnedAboutStatementsAfterReturn) {
        if (afterReturn) {
          if (!handler_.isStatementPermittedAfterReturnStatement(stmt)) {
            if (!warningAt(statementBegin, JSMSG_STMT_AFTER_RETURN)) {
              return errorResult();
            }

            warnedAboutStatementsAfterReturn = true;
          }
        } else if (handler_.isReturnStatement(stmt)) {
          afterReturn = true;
        }
      }
      handler_.addStatementToList(body, stmt);
    }

    CaseClauseType caseClause;
    MOZ_TRY_VAR(caseClause,
                handler_.newCaseOrDefault(caseBegin, caseExpr, body));
    handler_.addCaseStatementToList(caseList, caseClause);
  }

  LexicalScopeNodeType lexicalForCaseList;
  MOZ_TRY_VAR(lexicalForCaseList, finishLexicalScope(scope, caseList));

  handler_.setEndPosition(lexicalForCaseList, pos().end);

  return handler_.newSwitchStatement(begin, discriminant, lexicalForCaseList,
                                     seenDefault);
}

template <class ParseHandler, typename Unit>
typename ParseHandler::ContinueStatementResult
GeneralParser<ParseHandler, Unit>::continueStatement(
    YieldHandling yieldHandling) {
  MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::Continue));
  uint32_t begin = pos().begin;

  TaggedParserAtomIndex label;
  if (!matchLabel(yieldHandling, &label)) {
    return errorResult();
  }

  auto validity = pc_->checkContinueStatement(label);
  if (validity.isErr()) {
    switch (validity.unwrapErr()) {
      case ParseContext::ContinueStatementError::NotInALoop:
        errorAt(begin, JSMSG_BAD_CONTINUE);
        break;
      case ParseContext::ContinueStatementError::LabelNotFound:
        error(JSMSG_LABEL_NOT_FOUND);
        break;
    }
    return errorResult();
  }

  if (!matchOrInsertSemicolon()) {
    return errorResult();
  }

  return handler_.newContinueStatement(label, TokenPos(begin, pos().end));
}

template <class ParseHandler, typename Unit>
typename ParseHandler::BreakStatementResult
GeneralParser<ParseHandler, Unit>::breakStatement(YieldHandling yieldHandling) {
  MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::Break));
  uint32_t begin = pos().begin;

  TaggedParserAtomIndex label;
  if (!matchLabel(yieldHandling, &label)) {
    return errorResult();
  }

  auto validity = pc_->checkBreakStatement(label);
  if (validity.isErr()) {
    switch (validity.unwrapErr()) {
      case ParseContext::BreakStatementError::ToughBreak:
        errorAt(begin, JSMSG_TOUGH_BREAK);
        return errorResult();
      case ParseContext::BreakStatementError::LabelNotFound:
        error(JSMSG_LABEL_NOT_FOUND);
        return errorResult();
    }
  }

  if (!matchOrInsertSemicolon()) {
    return errorResult();
  }

  return handler_.newBreakStatement(label, TokenPos(begin, pos().end));
}

template <class ParseHandler, typename Unit>
typename ParseHandler::UnaryNodeResult
GeneralParser<ParseHandler, Unit>::returnStatement(
    YieldHandling yieldHandling) {
  MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::Return));
  uint32_t begin = pos().begin;

  MOZ_ASSERT(pc_->isFunctionBox());

  // Parse an optional operand.
  //
  // This is ugly, but we don't want to require a semicolon.
  Node exprNode;
  TokenKind tt = TokenKind::Eof;
  if (!tokenStream.peekTokenSameLine(&tt, TokenStream::SlashIsRegExp)) {
    return errorResult();
  }
  switch (tt) {
    case TokenKind::Eol:
    case TokenKind::Eof:
    case TokenKind::Semi:
    case TokenKind::RightCurly:
      exprNode = null();
      break;
    default: {
      MOZ_TRY_VAR(exprNode,
                  expr(InAllowed, yieldHandling, TripledotProhibited));
    }
  }

  if (!matchOrInsertSemicolon()) {
    return errorResult();
  }

  return handler_.newReturnStatement(exprNode, TokenPos(begin, pos().end));
}

template <class ParseHandler, typename Unit>
typename ParseHandler::UnaryNodeResult
GeneralParser<ParseHandler, Unit>::yieldExpression(InHandling inHandling) {
  MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::Yield));
  uint32_t begin = pos().begin;

  MOZ_ASSERT(pc_->isGenerator());
  MOZ_ASSERT(pc_->isFunctionBox());

  pc_->lastYieldOffset = begin;

  Node exprNode;
  ParseNodeKind kind = ParseNodeKind::YieldExpr;
  TokenKind tt = TokenKind::Eof;
  if (!tokenStream.peekTokenSameLine(&tt, TokenStream::SlashIsRegExp)) {
    return errorResult();
  }
  switch (tt) {
    // TokenKind::Eol is special; it implements the [no LineTerminator here]
    // quirk in the grammar.
    case TokenKind::Eol:
    // The rest of these make up the complete set of tokens that can
    // appear after any of the places where AssignmentExpression is used
    // throughout the grammar.  Conveniently, none of them can also be the
    // start an expression.
    case TokenKind::Eof:
    case TokenKind::Semi:
    case TokenKind::RightCurly:
    case TokenKind::RightBracket:
    case TokenKind::RightParen:
    case TokenKind::Colon:
    case TokenKind::Comma:
    case TokenKind::In:  // Annex B.3.6 `for (x = yield in y) ;`
      // No value.
      exprNode = null();
      break;
    case TokenKind::Mul:
      kind = ParseNodeKind::YieldStarExpr;
      tokenStream.consumeKnownToken(TokenKind::Mul, TokenStream::SlashIsRegExp);
      [[fallthrough]];
    default:
      MOZ_TRY_VAR(exprNode,
                  assignExpr(inHandling, YieldIsKeyword, TripledotProhibited));
  }
  if (kind == ParseNodeKind::YieldStarExpr) {
    return handler_.newYieldStarExpression(begin, exprNode);
  }
  return handler_.newYieldExpression(begin, exprNode);
}

template <class ParseHandler, typename Unit>
typename ParseHandler::BinaryNodeResult
GeneralParser<ParseHandler, Unit>::withStatement(YieldHandling yieldHandling) {
  MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::With));
  uint32_t begin = pos().begin;

  if (pc_->sc()->strict()) {
    if (!strictModeError(JSMSG_STRICT_CODE_WITH)) {
      return errorResult();
    }
  }

  if (!mustMatchToken(TokenKind::LeftParen, JSMSG_PAREN_BEFORE_WITH)) {
    return errorResult();
  }

  Node objectExpr;
  MOZ_TRY_VAR(objectExpr,
              exprInParens(InAllowed, yieldHandling, TripledotProhibited));

  if (!mustMatchToken(TokenKind::RightParen, JSMSG_PAREN_AFTER_WITH)) {
    return errorResult();
  }

  Node innerBlock;
  {
    ParseContext::Statement stmt(pc_, StatementKind::With);
    MOZ_TRY_VAR(innerBlock, statement(yieldHandling));
  }

  pc_->sc()->setBindingsAccessedDynamically();

  return handler_.newWithStatement(begin, objectExpr, innerBlock);
}

template <class ParseHandler, typename Unit>
typename ParseHandler::NodeResult
GeneralParser<ParseHandler, Unit>::labeledItem(YieldHandling yieldHandling) {
  TokenKind tt;
  if (!tokenStream.getToken(&tt, TokenStream::SlashIsRegExp)) {
    return errorResult();
  }

  if (tt == TokenKind::Function) {
    TokenKind next;
    if (!tokenStream.peekToken(&next)) {
      return errorResult();
    }

    // GeneratorDeclaration is only matched by HoistableDeclaration in
    // StatementListItem, so generators can't be inside labels.
    if (next == TokenKind::Mul) {
      error(JSMSG_GENERATOR_LABEL);
      return errorResult();
    }

    // Per 13.13.1 it's a syntax error if LabelledItem: FunctionDeclaration
    // is ever matched.  Per Annex B.3.2 that modifies this text, this
    // applies only to strict mode code.
    if (pc_->sc()->strict()) {
      error(JSMSG_FUNCTION_LABEL);
      return errorResult();
    }

    return functionStmt(pos().begin, yieldHandling, NameRequired);
  }

  anyChars.ungetToken();
  return statement(yieldHandling);
}

template <class ParseHandler, typename Unit>
typename ParseHandler::LabeledStatementResult
GeneralParser<ParseHandler, Unit>::labeledStatement(
    YieldHandling yieldHandling) {
  TaggedParserAtomIndex label = labelIdentifier(yieldHandling);
  if (!label) {
    return errorResult();
  }

  auto hasSameLabel = [&label](ParseContext::LabelStatement* stmt) {
    return stmt->label() == label;
  };

  uint32_t begin = pos().begin;

  if (pc_->template findInnermostStatement<ParseContext::LabelStatement>(
          hasSameLabel)) {
    errorAt(begin, JSMSG_DUPLICATE_LABEL);
    return errorResult();
  }

  tokenStream.consumeKnownToken(TokenKind::Colon);

  /* Push a label struct and parse the statement. */
  ParseContext::LabelStatement stmt(pc_, label);
  Node pn;
  MOZ_TRY_VAR(pn, labeledItem(yieldHandling));

  return handler_.newLabeledStatement(label, pn, begin);
}

template <class ParseHandler, typename Unit>
typename ParseHandler::UnaryNodeResult
GeneralParser<ParseHandler, Unit>::throwStatement(YieldHandling yieldHandling) {
  MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::Throw));
  uint32_t begin = pos().begin;

  /* ECMA-262 Edition 3 says 'throw [no LineTerminator here] Expr'. */
  TokenKind tt = TokenKind::Eof;
  if (!tokenStream.peekTokenSameLine(&tt, TokenStream::SlashIsRegExp)) {
    return errorResult();
  }
  if (tt == TokenKind::Eof || tt == TokenKind::Semi ||
      tt == TokenKind::RightCurly) {
    error(JSMSG_MISSING_EXPR_AFTER_THROW);
    return errorResult();
  }
  if (tt == TokenKind::Eol) {
    error(JSMSG_LINE_BREAK_AFTER_THROW);
    return errorResult();
  }

  Node throwExpr;
  MOZ_TRY_VAR(throwExpr, expr(InAllowed, yieldHandling, TripledotProhibited));

  if (!matchOrInsertSemicolon()) {
    return errorResult();
  }

  return handler_.newThrowStatement(throwExpr, TokenPos(begin, pos().end));
}

template <class ParseHandler, typename Unit>
typename ParseHandler::TernaryNodeResult
GeneralParser<ParseHandler, Unit>::tryStatement(YieldHandling yieldHandling) {
  MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::Try));
  uint32_t begin = pos().begin;

  /*
   * try nodes are ternary.
   * kid1 is the try statement
   * kid2 is the catch node list or null
   * kid3 is the finally statement
   *
   * catch nodes are binary.
   * left is the catch-name/pattern or null
   * right is the catch block
   *
   * catch lvalue nodes are either:
   *   a single identifier
   *   TokenKind::RightBracket for a destructuring left-hand side
   *   TokenKind::RightCurly for a destructuring left-hand side
   *
   * finally nodes are TokenKind::LeftCurly statement lists.
   */

  Node innerBlock;
  {
    if (!mustMatchToken(TokenKind::LeftCurly, JSMSG_CURLY_BEFORE_TRY)) {
      return errorResult();
    }

    uint32_t openedPos = pos().begin;

    ParseContext::Statement stmt(pc_, StatementKind::Try);
    ParseContext::Scope scope(this);
    if (!scope.init(pc_)) {
      return errorResult();
    }

    MOZ_TRY_VAR(innerBlock, statementList(yieldHandling));

    MOZ_TRY_VAR(innerBlock, finishLexicalScope(scope, innerBlock));

    if (!mustMatchToken(
            TokenKind::RightCurly, [this, openedPos](TokenKind actual) {
              this->reportMissingClosing(JSMSG_CURLY_AFTER_TRY,
                                         JSMSG_CURLY_OPENED, openedPos);
            })) {
      return errorResult();
    }
  }

  LexicalScopeNodeType catchScope = null();
  TokenKind tt;
  if (!tokenStream.getToken(&tt)) {
    return errorResult();
  }
  if (tt == TokenKind::Catch) {
    /*
     * Create a lexical scope node around the whole catch clause,
     * including the head.
     */
    ParseContext::Statement stmt(pc_, StatementKind::Catch);
    ParseContext::Scope scope(this);
    if (!scope.init(pc_)) {
      return errorResult();
    }

    /*
     * Legal catch forms are:
     *   catch (lhs) {
     *   catch {
     * where lhs is a name or a destructuring left-hand side.
     */
    bool omittedBinding;
    if (!tokenStream.matchToken(&omittedBinding, TokenKind::LeftCurly)) {
      return errorResult();
    }

    Node catchName;
    if (omittedBinding) {
      catchName = null();
    } else {
      if (!mustMatchToken(TokenKind::LeftParen, JSMSG_PAREN_BEFORE_CATCH)) {
        return errorResult();
      }

      if (!tokenStream.getToken(&tt)) {
        return errorResult();
      }
      switch (tt) {
        case TokenKind::LeftBracket:
        case TokenKind::LeftCurly:
          MOZ_TRY_VAR(catchName,
                      destructuringDeclaration(DeclarationKind::CatchParameter,
                                               yieldHandling, tt));
          break;

        default: {
          if (!TokenKindIsPossibleIdentifierName(tt)) {
            error(JSMSG_CATCH_IDENTIFIER);
            return errorResult();
          }

          MOZ_TRY_VAR(catchName,
                      bindingIdentifier(DeclarationKind::SimpleCatchParameter,
                                        yieldHandling));
          break;
        }
      }

      if (!mustMatchToken(TokenKind::RightParen, JSMSG_PAREN_AFTER_CATCH)) {
        return errorResult();
      }

      if (!mustMatchToken(TokenKind::LeftCurly, JSMSG_CURLY_BEFORE_CATCH)) {
        return errorResult();
      }
    }

    LexicalScopeNodeType catchBody;
    MOZ_TRY_VAR(catchBody, catchBlockStatement(yieldHandling, scope));

    MOZ_TRY_VAR(catchScope, finishLexicalScope(scope, catchBody));

    if (!handler_.setupCatchScope(catchScope, catchName, catchBody)) {
      return errorResult();
    }
    handler_.setEndPosition(catchScope, pos().end);

    if (!tokenStream.getToken(&tt, TokenStream::SlashIsRegExp)) {
      return errorResult();
    }
  }

  Node finallyBlock = null();

  if (tt == TokenKind::Finally) {
    if (!mustMatchToken(TokenKind::LeftCurly, JSMSG_CURLY_BEFORE_FINALLY)) {
      return errorResult();
    }

    uint32_t openedPos = pos().begin;

    ParseContext::Statement stmt(pc_, StatementKind::Finally);
    ParseContext::Scope scope(this);
    if (!scope.init(pc_)) {
      return errorResult();
    }

    MOZ_TRY_VAR(finallyBlock, statementList(yieldHandling));

    MOZ_TRY_VAR(finallyBlock, finishLexicalScope(scope, finallyBlock));

    if (!mustMatchToken(
            TokenKind::RightCurly, [this, openedPos](TokenKind actual) {
              this->reportMissingClosing(JSMSG_CURLY_AFTER_FINALLY,
                                         JSMSG_CURLY_OPENED, openedPos);
            })) {
      return errorResult();
    }
  } else {
    anyChars.ungetToken();
  }
  if (!catchScope && !finallyBlock) {
    error(JSMSG_CATCH_OR_FINALLY);
    return errorResult();
  }

  return handler_.newTryStatement(begin, innerBlock, catchScope, finallyBlock);
}

template <class ParseHandler, typename Unit>
typename ParseHandler::LexicalScopeNodeResult
GeneralParser<ParseHandler, Unit>::catchBlockStatement(
    YieldHandling yieldHandling, ParseContext::Scope& catchParamScope) {
  uint32_t openedPos = pos().begin;

  ParseContext::Statement stmt(pc_, StatementKind::Block);

  // ES 13.15.7 CatchClauseEvaluation
  //
  // Step 8 means that the body of a catch block always has an additional
  // lexical scope.
  ParseContext::Scope scope(this);
  if (!scope.init(pc_)) {
    return errorResult();
  }

  // The catch parameter names cannot be redeclared inside the catch
  // block, so declare the name in the inner scope.
  if (!scope.addCatchParameters(pc_, catchParamScope)) {
    return errorResult();
  }

  ListNodeType list;
  MOZ_TRY_VAR(list, statementList(yieldHandling));

  if (!mustMatchToken(
          TokenKind::RightCurly, [this, openedPos](TokenKind actual) {
            this->reportMissingClosing(JSMSG_CURLY_AFTER_CATCH,
                                       JSMSG_CURLY_OPENED, openedPos);
          })) {
    return errorResult();
  }

  // The catch parameter names are not bound in the body scope, so remove
  // them before generating bindings.
  scope.removeCatchParameters(pc_, catchParamScope);
  return finishLexicalScope(scope, list);
}

template <class ParseHandler, typename Unit>
typename ParseHandler::DebuggerStatementResult
GeneralParser<ParseHandler, Unit>::debuggerStatement() {
  TokenPos p;
  p.begin = pos().begin;
  if (!matchOrInsertSemicolon()) {
    return errorResult();
  }
  p.end = pos().end;

  return handler_.newDebuggerStatement(p);
}

static AccessorType ToAccessorType(PropertyType propType) {
  switch (propType) {
    case PropertyType::Getter:
      return AccessorType::Getter;
    case PropertyType::Setter:
      return AccessorType::Setter;
    case PropertyType::Normal:
    case PropertyType::Method:
    case PropertyType::GeneratorMethod:
    case PropertyType::AsyncMethod:
    case PropertyType::AsyncGeneratorMethod:
    case PropertyType::Constructor:
    case PropertyType::DerivedConstructor:
      return AccessorType::None;
    default:
      MOZ_CRASH("unexpected property type");
  }
}

#ifdef ENABLE_DECORATORS
template <class ParseHandler, typename Unit>
typename ParseHandler::ListNodeResult
GeneralParser<ParseHandler, Unit>::decoratorList(YieldHandling yieldHandling) {
  ListNodeType decorators;
  MOZ_TRY_VAR(decorators,
              handler_.newList(ParseNodeKind::DecoratorList, pos()));

  // Build a decorator list element. At each entry point to this loop we have
  // already consumed the |@| token
  TokenKind tt;
  for (;;) {
    if (!tokenStream.getToken(&tt, TokenStream::SlashIsInvalid)) {
      return errorResult();
    }

    Node decorator;
    MOZ_TRY_VAR(decorator, decoratorExpr(yieldHandling, tt));

    handler_.addList(decorators, decorator);

    if (!tokenStream.getToken(&tt)) {
      return errorResult();
    }
    if (tt != TokenKind::At) {
      anyChars.ungetToken();
      break;
    }
  }
  return decorators;
}
#endif

template <class ParseHandler, typename Unit>
bool GeneralParser<ParseHandler, Unit>::classMember(
    YieldHandling yieldHandling, const ParseContext::ClassStatement& classStmt,
    TaggedParserAtomIndex className, uint32_t classStartOffset,
    HasHeritage hasHeritage, ClassInitializedMembers& classInitializedMembers,
    ListNodeType& classMembers, bool* done) {
  *done = false;

  TokenKind tt;
  if (!tokenStream.getToken(&tt, TokenStream::SlashIsInvalid)) {
    return false;
  }
  if (tt == TokenKind::RightCurly) {
    *done = true;
    return true;
  }

  if (tt == TokenKind::Semi) {
    return true;
  }

#ifdef ENABLE_DECORATORS
  ListNodeType decorators = null();
  if (tt == TokenKind::At) {
    MOZ_TRY_VAR_OR_RETURN(decorators, decoratorList(yieldHandling), false);

    if (!tokenStream.getToken(&tt, TokenStream::SlashIsInvalid)) {
      return false;
    }
  }
#endif

  bool isStatic = false;
  if (tt == TokenKind::Static) {
    if (!tokenStream.peekToken(&tt)) {
      return false;
    }

    if (tt == TokenKind::LeftCurly) {
      /* Parsing static class block: static { ... } */
      FunctionNodeType staticBlockBody;
      MOZ_TRY_VAR_OR_RETURN(staticBlockBody,
                            staticClassBlock(classInitializedMembers), false);

      StaticClassBlockType classBlock;
      MOZ_TRY_VAR_OR_RETURN(
          classBlock, handler_.newStaticClassBlock(staticBlockBody), false);

      return handler_.addClassMemberDefinition(classMembers, classBlock);
    }

    if (tt != TokenKind::LeftParen && tt != TokenKind::Assign &&
        tt != TokenKind::Semi && tt != TokenKind::RightCurly) {
      isStatic = true;
    } else {
      anyChars.ungetToken();
    }
  } else {
    anyChars.ungetToken();
  }

  uint32_t propNameOffset;
  if (!tokenStream.peekOffset(&propNameOffset, TokenStream::SlashIsInvalid)) {
    return false;
  }

  TaggedParserAtomIndex propAtom;
  PropertyType propType;
  Node propName;
  MOZ_TRY_VAR_OR_RETURN(
      propName,
      propertyOrMethodName(yieldHandling, PropertyNameInClass,
                           /* maybeDecl = */ Nothing(), classMembers, &propType,
                           &propAtom),
      false);

  if (propType == PropertyType::Field ||
      propType == PropertyType::FieldWithAccessor) {
    if (isStatic) {
      if (propAtom == TaggedParserAtomIndex::WellKnown::prototype()) {
        errorAt(propNameOffset, JSMSG_BAD_METHOD_DEF);
        return false;
      }
    }

    if (propAtom == TaggedParserAtomIndex::WellKnown::constructor()) {
      errorAt(propNameOffset, JSMSG_BAD_METHOD_DEF);
      return false;
    }

    if (handler_.isPrivateName(propName)) {
      if (propAtom == TaggedParserAtomIndex::WellKnown::hash_constructor_()) {
        errorAt(propNameOffset, JSMSG_BAD_METHOD_DEF);
        return false;
      }

      auto privateName = propAtom;
      if (!noteDeclaredPrivateName(
              propName, privateName, propType,
              isStatic ? FieldPlacement::Static : FieldPlacement::Instance,
              pos())) {
        return false;
      }
    }

#ifdef ENABLE_DECORATORS
    ClassMethodType accessorGetterNode = null();
    ClassMethodType accessorSetterNode = null();
    if (propType == PropertyType::FieldWithAccessor) {
      // Decorators Proposal
      // https://arai-a.github.io/ecma262-compare/?pr=2417&id=sec-runtime-semantics-classfielddefinitionevaluation
      //
      // FieldDefinition : accessor ClassElementName Initializeropt
      //
      // Step 1. Let name be the result of evaluating ClassElementName.
      // ...
      // Step 3. Let privateStateDesc be the string-concatenation of name
      // and " accessor storage".
      StringBuffer privateStateDesc(fc_);
      if (!privateStateDesc.append(this->parserAtoms(), propAtom)) {
        return false;
      }
      if (!privateStateDesc.append(" accessor storage")) {
        return false;
      }
      // Step 4. Let privateStateName be a new Private Name whose
      // [[Description]] value is privateStateDesc.
      TokenPos propNamePos(propNameOffset, pos().end);
      auto privateStateName =
          privateStateDesc.finishParserAtom(this->parserAtoms(), fc_);
      if (!noteDeclaredPrivateName(
              propName, privateStateName, propType,
              isStatic ? FieldPlacement::Static : FieldPlacement::Instance,
              propNamePos)) {
        return false;
      }

      // Step 5. Let getter be MakeAutoAccessorGetter(homeObject, name,
      // privateStateName).
      MOZ_TRY_VAR_OR_RETURN(
          accessorGetterNode,
          synthesizeAccessor(propName, propNamePos, propAtom, privateStateName,
                             isStatic, FunctionSyntaxKind::Getter,
                             classInitializedMembers),
          false);

      // If the accessor is not decorated or is a non-static private field,
      // add it to the class here. Otherwise, we'll handle this when the
      // decorators are called. We don't need to keep a reference to the node
      // after this except for non-static private accessors. Please see the
      // comment in the definition of ClassField for details.
      bool addAccessorImmediately =
          !decorators || (!isStatic && handler_.isPrivateName(propName));
      if (addAccessorImmediately) {
        if (!handler_.addClassMemberDefinition(classMembers,
                                               accessorGetterNode)) {
          return false;
        }
        if (!handler_.isPrivateName(propName)) {
          accessorGetterNode = null();
        }
      }

      // Step 6. Let setter be MakeAutoAccessorSetter(homeObject, name,
      // privateStateName).
      MOZ_TRY_VAR_OR_RETURN(
          accessorSetterNode,
          synthesizeAccessor(propName, propNamePos, propAtom, privateStateName,
                             isStatic, FunctionSyntaxKind::Setter,
                             classInitializedMembers),
          false);

      if (addAccessorImmediately) {
        if (!handler_.addClassMemberDefinition(classMembers,
                                               accessorSetterNode)) {
          return false;
        }
        if (!handler_.isPrivateName(propName)) {
          accessorSetterNode = null();
        }
      }

      // Step 10. Return ClassElementDefinition Record { [[Key]]: name,
      // [[Kind]]: accessor, [[Get]]: getter, [[Set]]: setter,
      // [[BackingStorageKey]]: privateStateName, [[Initializers]]:
      // initializers, [[Decorators]]: empty }.
      MOZ_TRY_VAR_OR_RETURN(
          propName, handler_.newPrivateName(privateStateName, pos()), false);
      propAtom = privateStateName;
      // We maintain `decorators` here to perform this step at the same time:
      // https://arai-a.github.io/ecma262-compare/?pr=2417&id=sec-static-semantics-classelementevaluation
      // 4. Set fieldDefinition.[[Decorators]] to decorators.
    }
#endif
    if (isStatic) {
      classInitializedMembers.staticFields++;
    } else {
      classInitializedMembers.instanceFields++;
#ifdef ENABLE_DECORATORS
      if (decorators) {
        classInitializedMembers.hasInstanceDecorators = true;
      }
#endif
    }

    TokenPos propNamePos(propNameOffset, pos().end);
    FunctionNodeType initializer;
    MOZ_TRY_VAR_OR_RETURN(
        initializer,
        fieldInitializerOpt(propNamePos, propName, propAtom,
                            classInitializedMembers, isStatic, hasHeritage),
        false);

    if (!matchOrInsertSemicolon(TokenStream::SlashIsInvalid)) {
      return false;
    }

    ClassFieldType field;

    // MONGODB MODIFICATION: MSVC does not support using macros in the middle of functions.
#ifdef ENABLE_DECORATORS
    MOZ_TRY_VAR_OR_RETURN(field,
        handler_.newClassFieldDefinition(
            propName, initializer, isStatic, decorators, accessorGetterNode, accessorSetterNode),
        false);
#else
    MOZ_TRY_VAR_OR_RETURN(field,
        handler_.newClassFieldDefinition(propName, initializer, isStatic),
        false);
#endif

    return handler_.addClassMemberDefinition(classMembers, field);
  }

  if (propType != PropertyType::Getter && propType != PropertyType::Setter &&
      propType != PropertyType::Method &&
      propType != PropertyType::GeneratorMethod &&
      propType != PropertyType::AsyncMethod &&
      propType != PropertyType::AsyncGeneratorMethod) {
    errorAt(propNameOffset, JSMSG_BAD_METHOD_DEF);
    return false;
  }

  bool isConstructor =
      !isStatic && propAtom == TaggedParserAtomIndex::WellKnown::constructor();
  if (isConstructor) {
    if (propType != PropertyType::Method) {
      errorAt(propNameOffset, JSMSG_BAD_METHOD_DEF);
      return false;
    }
    if (classStmt.constructorBox) {
      errorAt(propNameOffset, JSMSG_DUPLICATE_PROPERTY, "constructor");
      return false;
    }
    propType = hasHeritage == HasHeritage::Yes
                   ? PropertyType::DerivedConstructor
                   : PropertyType::Constructor;
  } else if (isStatic &&
             propAtom == TaggedParserAtomIndex::WellKnown::prototype()) {
    errorAt(propNameOffset, JSMSG_BAD_METHOD_DEF);
    return false;
  }

  TaggedParserAtomIndex funName;
  switch (propType) {
    case PropertyType::Getter:
    case PropertyType::Setter: {
      bool hasStaticName =
          !anyChars.isCurrentTokenType(TokenKind::RightBracket) && propAtom;
      if (hasStaticName) {
        funName = prefixAccessorName(propType, propAtom);
        if (!funName) {
          return false;
        }
      }
      break;
    }
    case PropertyType::Constructor:
    case PropertyType::DerivedConstructor:
      funName = className;
      break;
    default:
      if (!anyChars.isCurrentTokenType(TokenKind::RightBracket)) {
        funName = propAtom;
      }
  }

  // When |super()| is invoked, we search for the nearest scope containing
  // |.initializers| to initialize the class fields. This set-up precludes
  // declaring |.initializers| in the class scope, because in some syntactic
  // contexts |super()| can appear nested in a class, while actually belonging
  // to an outer class definition.
  //
  // Example:
  // class Outer extends Base {
  //   field = 1;
  //   constructor() {
  //     class Inner {
  //       field = 2;
  //
  //       // The super() call in the computed property name mustn't access
  //       // Inner's |.initializers| array, but instead Outer's.
  //       [super()]() {}
  //     }
  //   }
  // }
  Maybe<ParseContext::Scope> dotInitializersScope;
  if (isConstructor && !options().selfHostingMode) {
    dotInitializersScope.emplace(this);
    if (!dotInitializersScope->init(pc_)) {
      return false;
    }

    if (!noteDeclaredName(TaggedParserAtomIndex::WellKnown::dot_initializers_(),
                          DeclarationKind::Let, pos())) {
      return false;
    }

#ifdef ENABLE_DECORATORS
    if (!noteDeclaredName(
            TaggedParserAtomIndex::WellKnown::dot_instanceExtraInitializers_(),
            DeclarationKind::Let, pos())) {
      return false;
    }
#endif
  }

  // Calling toString on constructors need to return the source text for
  // the entire class. The end offset is unknown at this point in
  // parsing and will be amended when class parsing finishes below.
  FunctionNodeType funNode;
  MOZ_TRY_VAR_OR_RETURN(
      funNode,
      methodDefinition(isConstructor ? classStartOffset : propNameOffset,
                       propType, funName),
      false);

  AccessorType atype = ToAccessorType(propType);

  Maybe<FunctionNodeType> initializerIfPrivate = Nothing();
  if (handler_.isPrivateName(propName)) {
    if (propAtom == TaggedParserAtomIndex::WellKnown::hash_constructor_()) {
      // #constructor is an invalid private name.
      errorAt(propNameOffset, JSMSG_BAD_METHOD_DEF);
      return false;
    }

    TaggedParserAtomIndex privateName = propAtom;
    if (!noteDeclaredPrivateName(
            propName, privateName, propType,
            isStatic ? FieldPlacement::Static : FieldPlacement::Instance,
            pos())) {
      return false;
    }

    // Private non-static methods are stored in the class body environment.
    // Private non-static accessors are stamped onto every instance using
    // initializers. Private static methods are stamped onto the constructor
    // during class evaluation; see BytecodeEmitter::emitPropertyList.
    if (!isStatic) {
      if (atype == AccessorType::Getter || atype == AccessorType::Setter) {
        classInitializedMembers.privateAccessors++;
        TokenPos propNamePos(propNameOffset, pos().end);
        FunctionNodeType initializerNode;
        MOZ_TRY_VAR_OR_RETURN(
            initializerNode,
            synthesizePrivateMethodInitializer(propAtom, atype, propNamePos),
            false);
        initializerIfPrivate = Some(initializerNode);
      } else {
        MOZ_ASSERT(atype == AccessorType::None);
        classInitializedMembers.privateMethods++;
      }
    }
  }

#ifdef ENABLE_DECORATORS
  if (decorators) {
    classInitializedMembers.hasInstanceDecorators = true;
  }
#endif

  Node method;

    // MONGODB MODIFICATION: MSVC does not support using macros in the middle of functions.
#ifdef ENABLE_DECORATORS
  MOZ_TRY_VAR_OR_RETURN(method,
      handler_.newClassMethodDefinition(propName, funNode, atype, isStatic, initializerIfPrivate, decorators),
      false);
#else
  MOZ_TRY_VAR_OR_RETURN(method,
      handler_.newClassMethodDefinition(propName, funNode, atype, isStatic, initializerIfPrivate),
      false);
#endif

  if (dotInitializersScope.isSome()) {
    MOZ_TRY_VAR_OR_RETURN(
        method, finishLexicalScope(*dotInitializersScope, method), false);
    dotInitializersScope.reset();
  }

  return handler_.addClassMemberDefinition(classMembers, method);
}

template <class ParseHandler, typename Unit>
bool GeneralParser<ParseHandler, Unit>::finishClassConstructor(
    const ParseContext::ClassStatement& classStmt,
    TaggedParserAtomIndex className, HasHeritage hasHeritage,
    uint32_t classStartOffset, uint32_t classEndOffset,
    const ClassInitializedMembers& classInitializedMembers,
    ListNodeType& classMembers) {
  if (classStmt.constructorBox == nullptr) {
    MOZ_ASSERT(!options().selfHostingMode);
    // Unconditionally create the scope here, because it's always the
    // constructor.
    ParseContext::Scope dotInitializersScope(this);
    if (!dotInitializersScope.init(pc_)) {
      return false;
    }

    if (!noteDeclaredName(TaggedParserAtomIndex::WellKnown::dot_initializers_(),
                          DeclarationKind::Let, pos())) {
      return false;
    }

#ifdef ENABLE_DECORATORS
    if (!noteDeclaredName(
            TaggedParserAtomIndex::WellKnown::dot_instanceExtraInitializers_(),
            DeclarationKind::Let, pos(), ClosedOver::Yes)) {
      return false;
    }
#endif

    // synthesizeConstructor assigns to classStmt.constructorBox
    TokenPos synthesizedBodyPos(classStartOffset, classEndOffset);
    FunctionNodeType synthesizedCtor;
    MOZ_TRY_VAR_OR_RETURN(
        synthesizedCtor,
        synthesizeConstructor(className, synthesizedBodyPos, hasHeritage),
        false);

    // Note: the *function* has the name of the class, but the *property*
    // containing the function has the name "constructor"
    Node constructorNameNode;
    MOZ_TRY_VAR_OR_RETURN(
        constructorNameNode,
        handler_.newObjectLiteralPropertyName(
            TaggedParserAtomIndex::WellKnown::constructor(), pos()),
        false);
    ClassMethodType method;
    MOZ_TRY_VAR_OR_RETURN(method,
                          handler_.newDefaultClassConstructor(
                              constructorNameNode, synthesizedCtor),
                          false);
    LexicalScopeNodeType scope;
    MOZ_TRY_VAR_OR_RETURN(
        scope, finishLexicalScope(dotInitializersScope, method), false);
    if (!handler_.addClassMemberDefinition(classMembers, scope)) {
      return false;
    }
  }

  MOZ_ASSERT(classStmt.constructorBox);
  FunctionBox* ctorbox = classStmt.constructorBox;

  // Amend the toStringEnd offset for the constructor now that we've
  // finished parsing the class.
  ctorbox->setCtorToStringEnd(classEndOffset);

  size_t numMemberInitializers = classInitializedMembers.privateAccessors +
                                 classInitializedMembers.instanceFields;
  bool hasPrivateBrand = classInitializedMembers.hasPrivateBrand();
  if (hasPrivateBrand || numMemberInitializers > 0) {
    // Now that we have full set of initializers, update the constructor.
    MemberInitializers initializers(
        hasPrivateBrand,
#ifdef ENABLE_DECORATORS
        classInitializedMembers.hasInstanceDecorators,
#endif
        numMemberInitializers);
    ctorbox->setMemberInitializers(initializers);

    // Field initialization need access to `this`.
    ctorbox->setCtorFunctionHasThisBinding();
  }

  return true;
}

template <class ParseHandler, typename Unit>
typename ParseHandler::ClassNodeResult
GeneralParser<ParseHandler, Unit>::classDefinition(
    YieldHandling yieldHandling, ClassContext classContext,
    DefaultHandling defaultHandling) {
#ifdef ENABLE_DECORATORS
  MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::At) ||
             anyChars.isCurrentTokenType(TokenKind::Class));

  ListNodeType decorators = null();
  FunctionNodeType addInitializerFunction = null();
  if (anyChars.isCurrentTokenType(TokenKind::At)) {
    MOZ_TRY_VAR(decorators, decoratorList(yieldHandling));
    TokenKind next;
    if (!tokenStream.getToken(&next)) {
      return errorResult();
    }
    if (next != TokenKind::Class) {
      error(JSMSG_CLASS_EXPECTED);
      return errorResult();
    }
  }
#else
  MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::Class));
#endif

  uint32_t classStartOffset = pos().begin;
  bool savedStrictness = setLocalStrictMode(true);

  // Classes are quite broken in self-hosted code.
  if (options().selfHostingMode) {
    error(JSMSG_SELFHOSTED_CLASS);
    return errorResult();
  }

  TokenKind tt;
  if (!tokenStream.getToken(&tt)) {
    return errorResult();
  }

  TaggedParserAtomIndex className;
  if (TokenKindIsPossibleIdentifier(tt)) {
    className = bindingIdentifier(yieldHandling);
    if (!className) {
      return errorResult();
    }
  } else if (classContext == ClassStatement) {
    if (defaultHandling == AllowDefaultName) {
      className = TaggedParserAtomIndex::WellKnown::default_();
      anyChars.ungetToken();
    } else {
      // Class statements must have a bound name
      error(JSMSG_UNNAMED_CLASS_STMT);
      return errorResult();
    }
  } else {
    // Make sure to put it back, whatever it was
    anyChars.ungetToken();
  }

  // Because the binding definitions keep track of their blockId, we need to
  // create at least the inner binding later. Keep track of the name's
  // position in order to provide it for the nodes created later.
  TokenPos namePos = pos();

  auto isClass = [](ParseContext::Statement* stmt) {
    return stmt->kind() == StatementKind::Class;
  };

  bool isInClass = pc_->sc()->inClass() || pc_->findInnermostStatement(isClass);

  // Push a ParseContext::ClassStatement to keep track of the constructor
  // funbox.
  ParseContext::ClassStatement classStmt(pc_);

  NameNodeType innerName;
  Node nameNode = null();
  Node classHeritage = null();
  LexicalScopeNodeType classBlock = null();
  ClassBodyScopeNodeType classBodyBlock = null();
  uint32_t classEndOffset;
  {
    // A named class creates a new lexical scope with a const binding of the
    // class name for the "inner name".
    ParseContext::Statement innerScopeStmt(pc_, StatementKind::Block);
    ParseContext::Scope innerScope(this);
    if (!innerScope.init(pc_)) {
      return errorResult();
    }

    bool hasHeritageBool;
    if (!tokenStream.matchToken(&hasHeritageBool, TokenKind::Extends)) {
      return errorResult();
    }
    HasHeritage hasHeritage =
        hasHeritageBool ? HasHeritage::Yes : HasHeritage::No;
    if (hasHeritage == HasHeritage::Yes) {
      if (!tokenStream.getToken(&tt)) {
        return errorResult();
      }
      MOZ_TRY_VAR(classHeritage,
                  optionalExpr(yieldHandling, TripledotProhibited, tt));
    }

    if (!mustMatchToken(TokenKind::LeftCurly, JSMSG_CURLY_BEFORE_CLASS)) {
      return errorResult();
    }

    {
      ParseContext::Statement bodyScopeStmt(pc_, StatementKind::Block);
      ParseContext::Scope bodyScope(this);
      if (!bodyScope.init(pc_)) {
        return errorResult();
      }

      ListNodeType classMembers;
      MOZ_TRY_VAR(classMembers, handler_.newClassMemberList(pos().begin));

      ClassInitializedMembers classInitializedMembers{};
      for (;;) {
        bool done;
        if (!classMember(yieldHandling, classStmt, className, classStartOffset,
                         hasHeritage, classInitializedMembers, classMembers,
                         &done)) {
          return errorResult();
        }
        if (done) {
          break;
        }
      }
#ifdef ENABLE_DECORATORS
      if (classInitializedMembers.hasInstanceDecorators) {
        MOZ_TRY_VAR(addInitializerFunction,
                    synthesizeAddInitializerFunction(
                        TaggedParserAtomIndex::WellKnown::
                            dot_instanceExtraInitializers_(),
                        yieldHandling));
      }
#endif

      if (classInitializedMembers.privateMethods +
              classInitializedMembers.privateAccessors >
          0) {
        // We declare `.privateBrand` as ClosedOver because the constructor
        // always uses it, even a default constructor. We could equivalently
        // `noteUsedName` when parsing the constructor, except that at that
        // time, we don't necessarily know if the class has a private brand.
        if (!noteDeclaredName(
                TaggedParserAtomIndex::WellKnown::dot_privateBrand_(),
                DeclarationKind::Synthetic, namePos, ClosedOver::Yes)) {
          return errorResult();
        }
      }

      if (classInitializedMembers.instanceFieldKeys > 0) {
        if (!noteDeclaredName(
                TaggedParserAtomIndex::WellKnown::dot_fieldKeys_(),
                DeclarationKind::Synthetic, namePos)) {
          return errorResult();
        }
      }

      if (classInitializedMembers.staticFields > 0) {
        if (!noteDeclaredName(
                TaggedParserAtomIndex::WellKnown::dot_staticInitializers_(),
                DeclarationKind::Synthetic, namePos)) {
          return errorResult();
        }
      }

      if (classInitializedMembers.staticFieldKeys > 0) {
        if (!noteDeclaredName(
                TaggedParserAtomIndex::WellKnown::dot_staticFieldKeys_(),
                DeclarationKind::Synthetic, namePos)) {
          return errorResult();
        }
      }

      classEndOffset = pos().end;
      if (!finishClassConstructor(classStmt, className, hasHeritage,
                                  classStartOffset, classEndOffset,
                                  classInitializedMembers, classMembers)) {
        return errorResult();
      }

      MOZ_TRY_VAR(classBodyBlock,
                  finishClassBodyScope(bodyScope, classMembers));

      // Pop the class body scope
    }

    if (className) {
      // The inner name is immutable.
      if (!noteDeclaredName(className, DeclarationKind::Const, namePos)) {
        return errorResult();
      }

      MOZ_TRY_VAR(innerName, newName(className, namePos));
    }

    MOZ_TRY_VAR(classBlock, finishLexicalScope(innerScope, classBodyBlock));

    // Pop the inner scope.
  }

  if (className) {
    NameNodeType outerName = null();
    if (classContext == ClassStatement) {
      // The outer name is mutable.
      if (!noteDeclaredName(className, DeclarationKind::Class, namePos)) {
        return errorResult();
      }

      MOZ_TRY_VAR(outerName, newName(className, namePos));
    }

    MOZ_TRY_VAR(nameNode,
                handler_.newClassNames(outerName, innerName, namePos));
  }
  MOZ_ALWAYS_TRUE(setLocalStrictMode(savedStrictness));
  // We're leaving a class definition that was not itself nested within a class
  if (!isInClass) {
    mozilla::Maybe<UnboundPrivateName> maybeUnboundName;
    if (!usedNames_.hasUnboundPrivateNames(fc_, maybeUnboundName)) {
      return errorResult();
    }
    if (maybeUnboundName) {
      UniqueChars str =
          this->parserAtoms().toPrintableString(maybeUnboundName->atom);
      if (!str) {
        ReportOutOfMemory(this->fc_);
        return errorResult();
      }

      errorAt(maybeUnboundName->position.begin, JSMSG_MISSING_PRIVATE_DECL,
              str.get());
      return errorResult();
    }
  }

  return handler_.newClass(nameNode, classHeritage, classBlock,
#ifdef ENABLE_DECORATORS
                           decorators, addInitializerFunction,
#endif
                           TokenPos(classStartOffset, classEndOffset));
}

template <class ParseHandler, typename Unit>
typename ParseHandler::FunctionNodeResult
GeneralParser<ParseHandler, Unit>::synthesizeConstructor(
    TaggedParserAtomIndex className, TokenPos synthesizedBodyPos,
    HasHeritage hasHeritage) {
  FunctionSyntaxKind functionSyntaxKind =
      hasHeritage == HasHeritage::Yes
          ? FunctionSyntaxKind::DerivedClassConstructor
          : FunctionSyntaxKind::ClassConstructor;

  bool isSelfHosting = options().selfHostingMode;
  FunctionFlags flags =
      InitialFunctionFlags(functionSyntaxKind, GeneratorKind::NotGenerator,
                           FunctionAsyncKind::SyncFunction, isSelfHosting);

  // Create the top-level field initializer node.
  FunctionNodeType funNode;
  MOZ_TRY_VAR(funNode,
              handler_.newFunction(functionSyntaxKind, synthesizedBodyPos));

  // If we see any inner function, note it on our current context. The bytecode
  // emitter may eliminate the function later, but we use a conservative
  // definition for consistency between lazy and full parsing.
  pc_->sc()->setHasInnerFunctions();

  // When fully parsing a lazy script, we do not fully reparse its inner
  // functions, which are also lazy. Instead, their free variables and source
  // extents are recorded and may be skipped.
  if (handler_.reuseLazyInnerFunctions()) {
    if (!skipLazyInnerFunction(funNode, synthesizedBodyPos.begin,
                               /* tryAnnexB = */ false)) {
      return errorResult();
    }

    return funNode;
  }

  // Create the FunctionBox and link it to the function object.
  Directives directives(true);
  FunctionBox* funbox = newFunctionBox(
      funNode, className, flags, synthesizedBodyPos.begin, directives,
      GeneratorKind::NotGenerator, FunctionAsyncKind::SyncFunction);
  if (!funbox) {
    return errorResult();
  }
  funbox->initWithEnclosingParseContext(pc_, functionSyntaxKind);
  setFunctionEndFromCurrentToken(funbox);

  // Mark this function as being synthesized by the parser. This means special
  // handling in delazification will be used since we don't have typical
  // function syntax.
  funbox->setSyntheticFunction();

  // Push a SourceParseContext on to the stack.
  ParseContext* outerpc = pc_;
  SourceParseContext funpc(this, funbox, /* newDirectives = */ nullptr);
  if (!funpc.init()) {
    return errorResult();
  }

  if (!synthesizeConstructorBody(synthesizedBodyPos, hasHeritage, funNode,
                                 funbox)) {
    return errorResult();
  }

  if (!leaveInnerFunction(outerpc)) {
    return errorResult();
  }

  return funNode;
}

template <class ParseHandler, typename Unit>
bool GeneralParser<ParseHandler, Unit>::synthesizeConstructorBody(
    TokenPos synthesizedBodyPos, HasHeritage hasHeritage,
    FunctionNodeType funNode, FunctionBox* funbox) {
  MOZ_ASSERT(funbox->isClassConstructor());

  // Create a ParamsBodyNode for the parameters + body (there are no
  // parameters).
  ParamsBodyNodeType argsbody;
  MOZ_TRY_VAR_OR_RETURN(argsbody, handler_.newParamsBody(synthesizedBodyPos),
                        false);
  handler_.setFunctionFormalParametersAndBody(funNode, argsbody);
  setFunctionStartAtPosition(funbox, synthesizedBodyPos);

  if (hasHeritage == HasHeritage::Yes) {
    // Synthesize the equivalent to `function f(...args)`
    funbox->setHasRest();
    if (!notePositionalFormalParameter(
            funNode, TaggedParserAtomIndex::WellKnown::dot_args_(),
            synthesizedBodyPos.begin,
            /* disallowDuplicateParams = */ false,
            /* duplicatedParam = */ nullptr)) {
      return false;
    }
    funbox->setArgCount(1);
  } else {
    funbox->setArgCount(0);
  }

  pc_->functionScope().useAsVarScope(pc_);

  ListNodeType stmtList;
  MOZ_TRY_VAR_OR_RETURN(stmtList, handler_.newStatementList(synthesizedBodyPos),
                        false);

  if (!noteUsedName(TaggedParserAtomIndex::WellKnown::dot_this_())) {
    return false;
  }

  if (!noteUsedName(TaggedParserAtomIndex::WellKnown::dot_initializers_())) {
    return false;
  }

#ifdef ENABLE_DECORATORS
  if (!noteUsedName(
          TaggedParserAtomIndex::WellKnown::dot_instanceExtraInitializers_())) {
    return false;
  }
#endif

  if (hasHeritage == HasHeritage::Yes) {
    // |super()| implicitly reads |new.target|.
    if (!noteUsedName(TaggedParserAtomIndex::WellKnown::dot_newTarget_())) {
      return false;
    }

    NameNodeType thisName;
    MOZ_TRY_VAR_OR_RETURN(thisName, newThisName(), false);

    UnaryNodeType superBase;
    MOZ_TRY_VAR_OR_RETURN(
        superBase, handler_.newSuperBase(thisName, synthesizedBodyPos), false);

    ListNodeType arguments;
    MOZ_TRY_VAR_OR_RETURN(arguments, handler_.newArguments(synthesizedBodyPos),
                          false);

    NameNodeType argsNameNode;
    MOZ_TRY_VAR_OR_RETURN(argsNameNode,
                          newName(TaggedParserAtomIndex::WellKnown::dot_args_(),
                                  synthesizedBodyPos),
                          false);
    if (!noteUsedName(TaggedParserAtomIndex::WellKnown::dot_args_())) {
      return false;
    }

    UnaryNodeType spreadArgs;
    MOZ_TRY_VAR_OR_RETURN(
        spreadArgs, handler_.newSpread(synthesizedBodyPos.begin, argsNameNode),
        false);
    handler_.addList(arguments, spreadArgs);

    CallNodeType superCall;
    MOZ_TRY_VAR_OR_RETURN(
        superCall,
        handler_.newSuperCall(superBase, arguments, /* isSpread = */ true),
        false);

    BinaryNodeType setThis;
    MOZ_TRY_VAR_OR_RETURN(setThis, handler_.newSetThis(thisName, superCall),
                          false);

    UnaryNodeType exprStatement;
    MOZ_TRY_VAR_OR_RETURN(
        exprStatement,
        handler_.newExprStatement(setThis, synthesizedBodyPos.end), false);

    handler_.addStatementToList(stmtList, exprStatement);
  }

  bool canSkipLazyClosedOverBindings = handler_.reuseClosedOverBindings();
  if (!pc_->declareFunctionThis(usedNames_, canSkipLazyClosedOverBindings)) {
    return false;
  }
  if (!pc_->declareNewTarget(usedNames_, canSkipLazyClosedOverBindings)) {
    return false;
  }

  LexicalScopeNodeType initializerBody;
  MOZ_TRY_VAR_OR_RETURN(
      initializerBody,
      finishLexicalScope(pc_->varScope(), stmtList, ScopeKind::FunctionLexical),
      false);
  handler_.setBeginPosition(initializerBody, stmtList);
  handler_.setEndPosition(initializerBody, stmtList);

  handler_.setFunctionBody(funNode, initializerBody);

  return finishFunction();
}

template <class ParseHandler, typename Unit>
typename ParseHandler::FunctionNodeResult
GeneralParser<ParseHandler, Unit>::privateMethodInitializer(
    TokenPos propNamePos, TaggedParserAtomIndex propAtom,
    TaggedParserAtomIndex storedMethodAtom) {
  if (!abortIfSyntaxParser()) {
    return errorResult();
  }

  // Synthesize an initializer function that the constructor can use to stamp a
  // private method onto an instance object.
  FunctionSyntaxKind syntaxKind = FunctionSyntaxKind::FieldInitializer;
  FunctionAsyncKind asyncKind = FunctionAsyncKind::SyncFunction;
  GeneratorKind generatorKind = GeneratorKind::NotGenerator;
  bool isSelfHosting = options().selfHostingMode;
  FunctionFlags flags =
      InitialFunctionFlags(syntaxKind, generatorKind, asyncKind, isSelfHosting);

  FunctionNodeType funNode;
  MOZ_TRY_VAR(funNode, handler_.newFunction(syntaxKind, propNamePos));

  Directives directives(true);
  FunctionBox* funbox =
      newFunctionBox(funNode, TaggedParserAtomIndex::null(), flags,
                     propNamePos.begin, directives, generatorKind, asyncKind);
  if (!funbox) {
    return errorResult();
  }
  funbox->initWithEnclosingParseContext(pc_, syntaxKind);

  // Push a SourceParseContext on to the stack.
  ParseContext* outerpc = pc_;
  SourceParseContext funpc(this, funbox, /* newDirectives = */ nullptr);
  if (!funpc.init()) {
    return errorResult();
  }
  pc_->functionScope().useAsVarScope(pc_);

  // Add empty parameter list.
  ParamsBodyNodeType argsbody;
  MOZ_TRY_VAR(argsbody, handler_.newParamsBody(propNamePos));
  handler_.setFunctionFormalParametersAndBody(funNode, argsbody);
  setFunctionStartAtCurrentToken(funbox);
  funbox->setArgCount(0);

  // Note both the stored private method body and it's private name as being
  // used in the initializer. They will be emitted into the method body in the
  // BCE.
  if (!noteUsedName(storedMethodAtom)) {
    return errorResult();
  }
  MOZ_TRY(privateNameReference(propAtom));

  // Unlike field initializers, private method initializers are not created with
  // a body of synthesized AST nodes. Instead, the body is left empty and the
  // initializer is synthesized at the bytecode level.
  // See BytecodeEmitter::emitPrivateMethodInitializer.
  ListNodeType stmtList;
  MOZ_TRY_VAR(stmtList, handler_.newStatementList(propNamePos));

  bool canSkipLazyClosedOverBindings = handler_.reuseClosedOverBindings();
  if (!pc_->declareFunctionThis(usedNames_, canSkipLazyClosedOverBindings)) {
    return errorResult();
  }
  if (!pc_->declareNewTarget(usedNames_, canSkipLazyClosedOverBindings)) {
    return errorResult();
  }

  LexicalScopeNodeType initializerBody;
  MOZ_TRY_VAR(initializerBody, finishLexicalScope(pc_->varScope(), stmtList,
                                                  ScopeKind::FunctionLexical));
  handler_.setBeginPosition(initializerBody, stmtList);
  handler_.setEndPosition(initializerBody, stmtList);
  handler_.setFunctionBody(funNode, initializerBody);

  // Set field-initializer lambda boundary to start at property name and end
  // after method body.
  setFunctionStartAtPosition(funbox, propNamePos);
  setFunctionEndFromCurrentToken(funbox);

  if (!finishFunction()) {
    return errorResult();
  }

  if (!leaveInnerFunction(outerpc)) {
    return errorResult();
  }

  return funNode;
}

template <class ParseHandler, typename Unit>
typename ParseHandler::FunctionNodeResult
GeneralParser<ParseHandler, Unit>::staticClassBlock(
    ClassInitializedMembers& classInitializedMembers) {
  // Both for getting-this-done, and because this will invariably be executed,
  // syntax parsing should be aborted.
  if (!abortIfSyntaxParser()) {
    return errorResult();
  }

  FunctionSyntaxKind syntaxKind = FunctionSyntaxKind::StaticClassBlock;
  FunctionAsyncKind asyncKind = FunctionAsyncKind::SyncFunction;
  GeneratorKind generatorKind = GeneratorKind::NotGenerator;
  bool isSelfHosting = options().selfHostingMode;
  FunctionFlags flags =
      InitialFunctionFlags(syntaxKind, generatorKind, asyncKind, isSelfHosting);

  AutoAwaitIsKeyword awaitIsKeyword(this, AwaitHandling::AwaitIsDisallowed);

  // Create the function node for the static class body.
  FunctionNodeType funNode;
  MOZ_TRY_VAR(funNode, handler_.newFunction(syntaxKind, pos()));

  // Create the FunctionBox and link it to the function object.
  Directives directives(true);
  FunctionBox* funbox =
      newFunctionBox(funNode, TaggedParserAtomIndex::null(), flags, pos().begin,
                     directives, generatorKind, asyncKind);
  if (!funbox) {
    return errorResult();
  }
  funbox->initWithEnclosingParseContext(pc_, syntaxKind);
  MOZ_ASSERT(funbox->isSyntheticFunction());
  MOZ_ASSERT(!funbox->allowSuperCall());
  MOZ_ASSERT(!funbox->allowArguments());
  MOZ_ASSERT(!funbox->allowReturn());

  // Set start at `static` token.
  MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::Static));
  setFunctionStartAtCurrentToken(funbox);

  // Push a SourceParseContext on to the stack.
  ParseContext* outerpc = pc_;
  SourceParseContext funpc(this, funbox, /* newDirectives = */ nullptr);
  if (!funpc.init()) {
    return errorResult();
  }

  pc_->functionScope().useAsVarScope(pc_);

  uint32_t start = pos().begin;

  tokenStream.consumeKnownToken(TokenKind::LeftCurly);

  // Static class blocks are code-generated as if they were static field
  // initializers, so we bump the staticFields count here, which ensures
  // .staticInitializers is noted as used.
  classInitializedMembers.staticFields++;

  LexicalScopeNodeType body;
  MOZ_TRY_VAR(body,
              functionBody(InHandling::InAllowed, YieldHandling::YieldIsKeyword,
                           syntaxKind, FunctionBodyType::StatementListBody));

  if (anyChars.isEOF()) {
    error(JSMSG_UNTERMINATED_STATIC_CLASS_BLOCK);
    return errorResult();
  }

  tokenStream.consumeKnownToken(TokenKind::RightCurly,
                                TokenStream::Modifier::SlashIsRegExp);

  TokenPos wholeBodyPos(start, pos().end);

  handler_.setEndPosition(funNode, wholeBodyPos.end);
  setFunctionEndFromCurrentToken(funbox);

  // Create a ParamsBodyNode for the parameters + body (there are no
  // parameters).
  ParamsBodyNodeType argsbody;
  MOZ_TRY_VAR(argsbody, handler_.newParamsBody(wholeBodyPos));

  handler_.setFunctionFormalParametersAndBody(funNode, argsbody);
  funbox->setArgCount(0);

  if (pc_->superScopeNeedsHomeObject()) {
    funbox->setNeedsHomeObject();
  }

  handler_.setEndPosition(body, pos().begin);
  handler_.setEndPosition(funNode, pos().end);
  handler_.setFunctionBody(funNode, body);

  if (!finishFunction()) {
    return errorResult();
  }

  if (!leaveInnerFunction(outerpc)) {
    return errorResult();
  }

  return funNode;
}

template <class ParseHandler, typename Unit>
typename ParseHandler::FunctionNodeResult
GeneralParser<ParseHandler, Unit>::fieldInitializerOpt(
    TokenPos propNamePos, Node propName, TaggedParserAtomIndex propAtom,
    ClassInitializedMembers& classInitializedMembers, bool isStatic,
    HasHeritage hasHeritage) {
  if (!abortIfSyntaxParser()) {
    return errorResult();
  }

  bool hasInitializer = false;
  if (!tokenStream.matchToken(&hasInitializer, TokenKind::Assign,
                              TokenStream::SlashIsDiv)) {
    return errorResult();
  }

  FunctionSyntaxKind syntaxKind = FunctionSyntaxKind::FieldInitializer;
  FunctionAsyncKind asyncKind = FunctionAsyncKind::SyncFunction;
  GeneratorKind generatorKind = GeneratorKind::NotGenerator;
  bool isSelfHosting = options().selfHostingMode;
  FunctionFlags flags =
      InitialFunctionFlags(syntaxKind, generatorKind, asyncKind, isSelfHosting);

  // Create the top-level field initializer node.
  FunctionNodeType funNode;
  MOZ_TRY_VAR(funNode, handler_.newFunction(syntaxKind, propNamePos));

  // Create the FunctionBox and link it to the function object.
  Directives directives(true);
  FunctionBox* funbox =
      newFunctionBox(funNode, TaggedParserAtomIndex::null(), flags,
                     propNamePos.begin, directives, generatorKind, asyncKind);
  if (!funbox) {
    return errorResult();
  }
  funbox->initWithEnclosingParseContext(pc_, syntaxKind);
  MOZ_ASSERT(funbox->isSyntheticFunction());

  // We can't use setFunctionStartAtCurrentToken because that uses pos().begin,
  // which is incorrect for fields without initializers (pos() points to the
  // field identifier)
  setFunctionStartAtPosition(funbox, propNamePos);

  // Push a SourceParseContext on to the stack.
  ParseContext* outerpc = pc_;
  SourceParseContext funpc(this, funbox, /* newDirectives = */ nullptr);
  if (!funpc.init()) {
    return errorResult();
  }

  pc_->functionScope().useAsVarScope(pc_);

  Node initializerExpr;
  if (hasInitializer) {
    // Parse the expression for the field initializer.
    {
      AutoAwaitIsKeyword awaitHandling(this, AwaitIsName);
      MOZ_TRY_VAR(initializerExpr,
                  assignExpr(InAllowed, YieldIsName, TripledotProhibited));
    }

    handler_.checkAndSetIsDirectRHSAnonFunction(initializerExpr);
  } else {
    MOZ_TRY_VAR(initializerExpr, handler_.newRawUndefinedLiteral(propNamePos));
  }

  TokenPos wholeInitializerPos(propNamePos.begin, pos().end);

  // Update the end position of the parse node.
  handler_.setEndPosition(funNode, wholeInitializerPos.end);
  setFunctionEndFromCurrentToken(funbox);

  // Create a ParamsBodyNode for the parameters + body (there are no
  // parameters).
  ParamsBodyNodeType argsbody;
  MOZ_TRY_VAR(argsbody, handler_.newParamsBody(wholeInitializerPos));
  handler_.setFunctionFormalParametersAndBody(funNode, argsbody);
  funbox->setArgCount(0);

  NameNodeType thisName;
  MOZ_TRY_VAR(thisName, newThisName());

  // Build `this.field` expression.
  ThisLiteralType propAssignThis;
  MOZ_TRY_VAR(propAssignThis,
              handler_.newThisLiteral(wholeInitializerPos, thisName));

  Node propAssignFieldAccess;
  uint32_t indexValue;
  if (!propAtom) {
    // See BytecodeEmitter::emitCreateFieldKeys for an explanation of what
    // .fieldKeys means and its purpose.
    NameNodeType fieldKeysName;
    if (isStatic) {
      MOZ_TRY_VAR(
          fieldKeysName,
          newInternalDotName(
              TaggedParserAtomIndex::WellKnown::dot_staticFieldKeys_()));
    } else {
      MOZ_TRY_VAR(fieldKeysName,
                  newInternalDotName(
                      TaggedParserAtomIndex::WellKnown::dot_fieldKeys_()));
    }
    if (!fieldKeysName) {
      return errorResult();
    }

    double fieldKeyIndex;
    if (isStatic) {
      fieldKeyIndex = classInitializedMembers.staticFieldKeys++;
    } else {
      fieldKeyIndex = classInitializedMembers.instanceFieldKeys++;
    }
    Node fieldKeyIndexNode;
    MOZ_TRY_VAR(fieldKeyIndexNode,
                handler_.newNumber(fieldKeyIndex, DecimalPoint::NoDecimal,
                                   wholeInitializerPos));

    Node fieldKeyValue;
    MOZ_TRY_VAR(fieldKeyValue,
                handler_.newPropertyByValue(fieldKeysName, fieldKeyIndexNode,
                                            wholeInitializerPos.end));

    MOZ_TRY_VAR(propAssignFieldAccess,
                handler_.newPropertyByValue(propAssignThis, fieldKeyValue,
                                            wholeInitializerPos.end));
  } else if (handler_.isPrivateName(propName)) {
    // It would be nice if we could tweak this here such that only if
    // HasHeritage::Yes we end up emitting CheckPrivateField, but otherwise we
    // emit InitElem -- this is an optimization to minimize HasOwn checks
    // in InitElem for classes without heritage.
    //
    // Further tweaking would be to ultimately only do CheckPrivateField for the
    // -first- field in a derived class, which would suffice to match the
    // semantic check.

    NameNodeType privateNameNode;
    MOZ_TRY_VAR(privateNameNode, privateNameReference(propAtom));

    MOZ_TRY_VAR(propAssignFieldAccess,
                handler_.newPrivateMemberAccess(propAssignThis, privateNameNode,
                                                wholeInitializerPos.end));
  } else if (this->parserAtoms().isIndex(propAtom, &indexValue)) {
    MOZ_TRY_VAR(propAssignFieldAccess,
                handler_.newPropertyByValue(propAssignThis, propName,
                                            wholeInitializerPos.end));
  } else {
    NameNodeType propAssignName;
    MOZ_TRY_VAR(propAssignName,
                handler_.newPropertyName(propAtom, wholeInitializerPos));

    MOZ_TRY_VAR(propAssignFieldAccess,
                handler_.newPropertyAccess(propAssignThis, propAssignName));
  }

  // Synthesize an property init.
  BinaryNodeType initializerPropInit;
  MOZ_TRY_VAR(initializerPropInit,
              handler_.newInitExpr(propAssignFieldAccess, initializerExpr));

  UnaryNodeType exprStatement;
  MOZ_TRY_VAR(exprStatement, handler_.newExprStatement(
                                 initializerPropInit, wholeInitializerPos.end));

  ListNodeType statementList;
  MOZ_TRY_VAR(statementList, handler_.newStatementList(wholeInitializerPos));
  handler_.addStatementToList(statementList, exprStatement);

  bool canSkipLazyClosedOverBindings = handler_.reuseClosedOverBindings();
  if (!pc_->declareFunctionThis(usedNames_, canSkipLazyClosedOverBindings)) {
    return errorResult();
  }
  if (!pc_->declareNewTarget(usedNames_, canSkipLazyClosedOverBindings)) {
    return errorResult();
  }

  // Set the function's body to the field assignment.
  LexicalScopeNodeType initializerBody;
  MOZ_TRY_VAR(initializerBody,
              finishLexicalScope(pc_->varScope(), statementList,
                                 ScopeKind::FunctionLexical));

  handler_.setFunctionBody(funNode, initializerBody);

  if (pc_->superScopeNeedsHomeObject()) {
    funbox->setNeedsHomeObject();
  }

  if (!finishFunction()) {
    return errorResult();
  }

  if (!leaveInnerFunction(outerpc)) {
    return errorResult();
  }

  return funNode;
}

template <class ParseHandler, typename Unit>
typename ParseHandler::FunctionNodeResult
GeneralParser<ParseHandler, Unit>::synthesizePrivateMethodInitializer(
    TaggedParserAtomIndex propAtom, AccessorType accessorType,
    TokenPos propNamePos) {
  if (!abortIfSyntaxParser()) {
    return errorResult();
  }

  // Synthesize a name for the lexical variable that will store the
  // accessor body.
  StringBuffer storedMethodName(fc_);
  if (!storedMethodName.append(this->parserAtoms(), propAtom)) {
    return errorResult();
  }
  if (!((accessorType == AccessorType::Getter)
            ? storedMethodName.append(".getter")
            : storedMethodName.append(".setter"))) {
    return errorResult();
  }
  auto storedMethodProp =
      storedMethodName.finishParserAtom(this->parserAtoms(), fc_);
  if (!storedMethodProp) {
    return errorResult();
  }
  if (!noteDeclaredName(storedMethodProp, DeclarationKind::Synthetic, pos())) {
    return errorResult();
  }

  return privateMethodInitializer(propNamePos, propAtom, storedMethodProp);
}

#ifdef ENABLE_DECORATORS
template <class ParseHandler, typename Unit>
typename ParseHandler::FunctionNodeResult
GeneralParser<ParseHandler, Unit>::synthesizeAddInitializerFunction(
    TaggedParserAtomIndex initializers, YieldHandling yieldHandling) {
  if (!abortIfSyntaxParser()) {
    return errorResult();
  }

  // TODO: Add support for static and class extra initializers, see bug 1868220
  // and bug 1868221.
  MOZ_ASSERT(
      initializers ==
      TaggedParserAtomIndex::WellKnown::dot_instanceExtraInitializers_());

  TokenPos propNamePos = pos();

  // Synthesize an addInitializer function that can be used to append to
  // .initializers
  FunctionSyntaxKind syntaxKind = FunctionSyntaxKind::Statement;
  FunctionAsyncKind asyncKind = FunctionAsyncKind::SyncFunction;
  GeneratorKind generatorKind = GeneratorKind::NotGenerator;
  bool isSelfHosting = options().selfHostingMode;
  FunctionFlags flags =
      InitialFunctionFlags(syntaxKind, generatorKind, asyncKind, isSelfHosting);

  FunctionNodeType funNode;
  MOZ_TRY_VAR(funNode, handler_.newFunction(syntaxKind, propNamePos));

  Directives directives(true);
  FunctionBox* funbox =
      newFunctionBox(funNode, TaggedParserAtomIndex::null(), flags,
                     propNamePos.begin, directives, generatorKind, asyncKind);
  if (!funbox) {
    return errorResult();
  }
  funbox->initWithEnclosingParseContext(pc_, syntaxKind);

  ParseContext* outerpc = pc_;
  SourceParseContext funpc(this, funbox, /* newDirectives = */ nullptr);
  if (!funpc.init()) {
    return errorResult();
  }
  pc_->functionScope().useAsVarScope(pc_);

  // Takes a single parameter, `initializer`.
  ParamsBodyNodeType params;
  MOZ_TRY_VAR(params, handler_.newParamsBody(propNamePos));

  handler_.setFunctionFormalParametersAndBody(funNode, params);

  constexpr bool disallowDuplicateParams = true;
  bool duplicatedParam = false;
  if (!notePositionalFormalParameter(
          funNode, TaggedParserAtomIndex::WellKnown::initializer(), pos().begin,
          disallowDuplicateParams, &duplicatedParam)) {
    return null();
  }
  MOZ_ASSERT(!duplicatedParam);
  MOZ_ASSERT(pc_->positionalFormalParameterNames().length() == 1);

  funbox->setLength(1);
  funbox->setArgCount(1);
  setFunctionStartAtCurrentToken(funbox);

  // Like private method initializers, the addInitializer method is not created
  // with a body of synthesized AST nodes. Instead, the body is left empty and
  // the initializer is synthesized at the bytecode level. See
  // DecoratorEmitter::emitCreateAddInitializerFunction.
  ListNodeType stmtList;
  MOZ_TRY_VAR(stmtList, handler_.newStatementList(propNamePos));

  if (!noteUsedName(initializers)) {
    return null();
  }

  bool canSkipLazyClosedOverBindings = handler_.reuseClosedOverBindings();
  if (!pc_->declareFunctionThis(usedNames_, canSkipLazyClosedOverBindings)) {
    return null();
  }
  if (!pc_->declareNewTarget(usedNames_, canSkipLazyClosedOverBindings)) {
    return null();
  }

  LexicalScopeNodeType addInitializerBody;
  MOZ_TRY_VAR(addInitializerBody,
              finishLexicalScope(pc_->varScope(), stmtList,
                                 ScopeKind::FunctionLexical));
  handler_.setBeginPosition(addInitializerBody, stmtList);
  handler_.setEndPosition(addInitializerBody, stmtList);
  handler_.setFunctionBody(funNode, addInitializerBody);

  // Set field-initializer lambda boundary to start at property name and end
  // after method body.
  setFunctionStartAtPosition(funbox, propNamePos);
  setFunctionEndFromCurrentToken(funbox);

  if (!finishFunction()) {
    return errorResult();
  }

  if (!leaveInnerFunction(outerpc)) {
    return errorResult();
  }

  return funNode;
}

template <class ParseHandler, typename Unit>
typename ParseHandler::ClassMethodResult
GeneralParser<ParseHandler, Unit>::synthesizeAccessor(
    Node propName, TokenPos propNamePos, TaggedParserAtomIndex propAtom,
    TaggedParserAtomIndex privateStateNameAtom, bool isStatic,
    FunctionSyntaxKind syntaxKind,
    ClassInitializedMembers& classInitializedMembers) {
  // Decorators Proposal
  // https://arai-a.github.io/ecma262-compare/?pr=2417&id=sec-makeautoaccessorgetter
  // The abstract operation MakeAutoAccessorGetter takes arguments homeObject
  // (an Object), name (a property key or Private Name), and privateStateName (a
  // Private Name) and returns a function object.
  //
  // https://arai-a.github.io/ecma262-compare/?pr=2417&id=sec-makeautoaccessorsetter
  // The abstract operation MakeAutoAccessorSetter takes arguments homeObject
  // (an Object), name (a property key or Private Name), and privateStateName (a
  // Private Name) and returns a function object.
  if (!abortIfSyntaxParser()) {
    return errorResult();
  }

  AccessorType accessorType = syntaxKind == FunctionSyntaxKind::Getter
                                  ? AccessorType::Getter
                                  : AccessorType::Setter;

  mozilla::Maybe<FunctionNodeType> initializerIfPrivate = Nothing();
  if (!isStatic && handler_.isPrivateName(propName)) {
    classInitializedMembers.privateAccessors++;
    FunctionNodeType initializerNode;
    MOZ_TRY_VAR(initializerNode, synthesizePrivateMethodInitializer(
                                     propAtom, accessorType, propNamePos));
    initializerIfPrivate = Some(initializerNode);
    handler_.setPrivateNameKind(propName, PrivateNameKind::GetterSetter);
  }

  // https://arai-a.github.io/ecma262-compare/?pr=2417&id=sec-makeautoaccessorgetter
  // 2. Let getter be CreateBuiltinFunction(getterClosure, 0, "get", « »).
  //
  // https://arai-a.github.io/ecma262-compare/?pr=2417&id=sec-makeautoaccessorsetter
  // 2. Let setter be CreateBuiltinFunction(setterClosure, 1, "set", « »).
  StringBuffer storedMethodName(fc_);
  if (!storedMethodName.append(accessorType == AccessorType::Getter ? "get"
                                                                    : "set")) {
    return errorResult();
  }
  TaggedParserAtomIndex funNameAtom =
      storedMethodName.finishParserAtom(this->parserAtoms(), fc_);

  FunctionNodeType funNode;
  MOZ_TRY_VAR(funNode,
              synthesizeAccessorBody(funNameAtom, propNamePos,
                                     privateStateNameAtom, syntaxKind));

  // https://arai-a.github.io/ecma262-compare/?pr=2417&id=sec-makeautoaccessorgetter
  // 3. Perform MakeMethod(getter, homeObject).
  // 4. Return getter.
  //
  // https://arai-a.github.io/ecma262-compare/?pr=2417&id=sec-makeautoaccessorsetter
  // 3. Perform MakeMethod(setter, homeObject).
  // 4. Return setter.
  return handler_.newClassMethodDefinition(
      propName, funNode, accessorType, isStatic, initializerIfPrivate, null());
}

template <class ParseHandler, typename Unit>
typename ParseHandler::FunctionNodeResult
GeneralParser<ParseHandler, Unit>::synthesizeAccessorBody(
    TaggedParserAtomIndex funNameAtom, TokenPos propNamePos,
    TaggedParserAtomIndex propNameAtom, FunctionSyntaxKind syntaxKind) {
  if (!abortIfSyntaxParser()) {
    return errorResult();
  }

  FunctionAsyncKind asyncKind = FunctionAsyncKind::SyncFunction;
  GeneratorKind generatorKind = GeneratorKind::NotGenerator;
  bool isSelfHosting = options().selfHostingMode;
  FunctionFlags flags =
      InitialFunctionFlags(syntaxKind, generatorKind, asyncKind, isSelfHosting);

  // Create the top-level function node.
  FunctionNodeType funNode;
  MOZ_TRY_VAR(funNode, handler_.newFunction(syntaxKind, propNamePos));

  // Create the FunctionBox and link it to the function object.
  Directives directives(true);
  FunctionBox* funbox =
      newFunctionBox(funNode, funNameAtom, flags, propNamePos.begin, directives,
                     generatorKind, asyncKind);
  if (!funbox) {
    return errorResult();
  }
  funbox->initWithEnclosingParseContext(pc_, syntaxKind);
  funbox->setSyntheticFunction();

  // Push a SourceParseContext on to the stack.
  ParseContext* outerpc = pc_;
  SourceParseContext funpc(this, funbox, /* newDirectives = */ nullptr);
  if (!funpc.init()) {
    return errorResult();
  }

  pc_->functionScope().useAsVarScope(pc_);

  // The function we synthesize is located at the field with the
  // accessor.
  setFunctionStartAtCurrentToken(funbox);
  setFunctionEndFromCurrentToken(funbox);

  // Create a ListNode for the parameters + body
  ParamsBodyNodeType paramsbody;
  MOZ_TRY_VAR(paramsbody, handler_.newParamsBody(propNamePos));
  handler_.setFunctionFormalParametersAndBody(funNode, paramsbody);

  if (syntaxKind == FunctionSyntaxKind::Getter) {
    funbox->setArgCount(0);
  } else {
    funbox->setArgCount(1);
  }

  // Build `this` expression to access the privateStateName for use in the
  // operations to create the getter and setter below.
  NameNodeType thisName;
  MOZ_TRY_VAR(thisName, newThisName());

  ThisLiteralType propThis;
  MOZ_TRY_VAR(propThis, handler_.newThisLiteral(propNamePos, thisName));

  NameNodeType privateNameNode;
  MOZ_TRY_VAR(privateNameNode, privateNameReference(propNameAtom));

  Node propFieldAccess;
  MOZ_TRY_VAR(propFieldAccess, handler_.newPrivateMemberAccess(
                                   propThis, privateNameNode, propNamePos.end));

  Node accessorBody;
  if (syntaxKind == FunctionSyntaxKind::Getter) {
    // Decorators Proposal
    // https://arai-a.github.io/ecma262-compare/?pr=2417&id=sec-makeautoaccessorgetter
    // 1. Let getterClosure be a new Abstract Closure with no parameters that
    // captures privateStateName and performs the following steps when called:
    //  1.a. Let o be the this value.
    //  1.b. Return ? PrivateGet(privateStateName, o).
    MOZ_TRY_VAR(accessorBody,
                handler_.newReturnStatement(propFieldAccess, propNamePos));
  } else {
    // Decorators Proposal
    // https://arai-a.github.io/ecma262-compare/?pr=2417&id=sec-makeautoaccessorsetter
    // The abstract operation MakeAutoAccessorSetter takes arguments homeObject
    // (an Object), name (a property key or Private Name), and privateStateName
    // (a Private Name) and returns a function object.
    // 1. Let setterClosure be a new Abstract Closure with parameters (value)
    // that captures privateStateName and performs the following steps when
    // called:
    //   1.a. Let o be the this value.
    notePositionalFormalParameter(funNode,
                                  TaggedParserAtomIndex::WellKnown::value(),
                                  /* pos = */ 0, false,
                                  /* duplicatedParam = */ nullptr);

    Node initializerExpr;
    MOZ_TRY_VAR(initializerExpr,
                handler_.newName(TaggedParserAtomIndex::WellKnown::value(),
                                 propNamePos));

    //   1.b. Perform ? PrivateSet(privateStateName, o, value).
    Node assignment;
    MOZ_TRY_VAR(assignment,
                handler_.newAssignment(ParseNodeKind::AssignExpr,
                                       propFieldAccess, initializerExpr));

    MOZ_TRY_VAR(accessorBody,
                handler_.newExprStatement(assignment, propNamePos.end));

    //   1.c. Return undefined.
  }

  ListNodeType statementList;
  MOZ_TRY_VAR(statementList, handler_.newStatementList(propNamePos));
  handler_.addStatementToList(statementList, accessorBody);

  bool canSkipLazyClosedOverBindings = handler_.reuseClosedOverBindings();
  if (!pc_->declareFunctionThis(usedNames_, canSkipLazyClosedOverBindings)) {
    return errorResult();
  }
  if (!pc_->declareNewTarget(usedNames_, canSkipLazyClosedOverBindings)) {
    return errorResult();
  }

  LexicalScopeNodeType initializerBody;
  MOZ_TRY_VAR(initializerBody,
              finishLexicalScope(pc_->varScope(), statementList,
                                 ScopeKind::FunctionLexical));

  handler_.setFunctionBody(funNode, initializerBody);

  if (pc_->superScopeNeedsHomeObject()) {
    funbox->setNeedsHomeObject();
  }

  if (!finishFunction()) {
    return errorResult();
  }

  if (!leaveInnerFunction(outerpc)) {
    return errorResult();
  }

  return funNode;
}

#endif

bool ParserBase::nextTokenContinuesLetDeclaration(TokenKind next) {
  MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::Let));
  MOZ_ASSERT(anyChars.nextToken().type == next);

  TokenStreamShared::verifyConsistentModifier(TokenStreamShared::SlashIsDiv,
                                              anyChars.nextToken());

  // Destructuring continues a let declaration.
  if (next == TokenKind::LeftBracket || next == TokenKind::LeftCurly) {
    return true;
  }

  // A "let" edge case deserves special comment.  Consider this:
  //
  //   let     // not an ASI opportunity
  //   let;
  //
  // Static semantics in §13.3.1.1 turn a LexicalDeclaration that binds
  // "let" into an early error.  Does this retroactively permit ASI so
  // that we should parse this as two ExpressionStatements?   No.  ASI
  // resolves during parsing.  Static semantics only apply to the full
  // parse tree with ASI applied.  No backsies!

  // Otherwise a let declaration must have a name.
  return TokenKindIsPossibleIdentifier(next);
}

template <class ParseHandler, typename Unit>
typename ParseHandler::DeclarationListNodeResult
GeneralParser<ParseHandler, Unit>::variableStatement(
    YieldHandling yieldHandling) {
  DeclarationListNodeType vars;
  MOZ_TRY_VAR(vars, declarationList(yieldHandling, ParseNodeKind::VarStmt));
  if (!matchOrInsertSemicolon()) {
    return errorResult();
  }
  return vars;
}

template <class ParseHandler, typename Unit>
typename ParseHandler::NodeResult GeneralParser<ParseHandler, Unit>::statement(
    YieldHandling yieldHandling) {
  MOZ_ASSERT(checkOptionsCalled_);

  AutoCheckRecursionLimit recursion(this->fc_);
  if (!recursion.check(this->fc_)) {
    return errorResult();
  }

  TokenKind tt;
  if (!tokenStream.getToken(&tt, TokenStream::SlashIsRegExp)) {
    return errorResult();
  }

  switch (tt) {
    // BlockStatement[?Yield, ?Return]
    case TokenKind::LeftCurly:
      return blockStatement(yieldHandling);

    // VariableStatement[?Yield]
    case TokenKind::Var:
      return variableStatement(yieldHandling);

    // EmptyStatement
    case TokenKind::Semi:
      return handler_.newEmptyStatement(pos());

      // ExpressionStatement[?Yield].

    case TokenKind::Yield: {
      // Don't use a ternary operator here due to obscure linker issues
      // around using static consts in the arms of a ternary.
      Modifier modifier;
      if (yieldExpressionsSupported()) {
        modifier = TokenStream::SlashIsRegExp;
      } else {
        modifier = TokenStream::SlashIsDiv;
      }

      TokenKind next;
      if (!tokenStream.peekToken(&next, modifier)) {
        return errorResult();
      }

      if (next == TokenKind::Colon) {
        return labeledStatement(yieldHandling);
      }

      return expressionStatement(yieldHandling);
    }

    default: {
      // If we encounter an await in a module, and the module is not marked
      // as async, mark the module as async.
      if (tt == TokenKind::Await && !pc_->isAsync()) {
        if (pc_->atModuleTopLevel()) {
          if (!options().topLevelAwait) {
            error(JSMSG_TOP_LEVEL_AWAIT_NOT_SUPPORTED);
            return errorResult();
          }
          pc_->sc()->asModuleContext()->setIsAsync();
          MOZ_ASSERT(pc_->isAsync());
        }
      }

      // Avoid getting next token with SlashIsDiv.
      if (tt == TokenKind::Await && pc_->isAsync()) {
        return expressionStatement(yieldHandling);
      }

      if (!TokenKindIsPossibleIdentifier(tt)) {
        return expressionStatement(yieldHandling);
      }

      TokenKind next;
      if (!tokenStream.peekToken(&next)) {
        return errorResult();
      }

      // |let| here can only be an Identifier, not a declaration.  Give nicer
      // errors for declaration-looking typos.
      if (tt == TokenKind::Let) {
        bool forbiddenLetDeclaration = false;

        if (next == TokenKind::LeftBracket) {
          // Enforce ExpressionStatement's 'let [' lookahead restriction.
          forbiddenLetDeclaration = true;
        } else if (next == TokenKind::LeftCurly ||
                   TokenKindIsPossibleIdentifier(next)) {
          // 'let {' and 'let foo' aren't completely forbidden, if ASI
          // causes 'let' to be the entire Statement.  But if they're
          // same-line, we can aggressively give a better error message.
          //
          // Note that this ignores 'yield' as TokenKind::Yield: we'll handle it
          // correctly but with a worse error message.
          TokenKind nextSameLine;
          if (!tokenStream.peekTokenSameLine(&nextSameLine)) {
            return errorResult();
          }

          MOZ_ASSERT(TokenKindIsPossibleIdentifier(nextSameLine) ||
                     nextSameLine == TokenKind::LeftCurly ||
                     nextSameLine == TokenKind::Eol);

          forbiddenLetDeclaration = nextSameLine != TokenKind::Eol;
        }

        if (forbiddenLetDeclaration) {
          error(JSMSG_FORBIDDEN_AS_STATEMENT, "lexical declarations");
          return errorResult();
        }
      } else if (tt == TokenKind::Async) {
        // Peek only on the same line: ExpressionStatement's lookahead
        // restriction is phrased as
        //
        //   [lookahead ∉ { '{',
        //                  function,
        //                  async [no LineTerminator here] function,
        //                  class,
        //                  let '[' }]
        //
        // meaning that code like this is valid:
        //
        //   if (true)
        //     async       // ASI opportunity
        //   function clownshoes() {}
        TokenKind maybeFunction;
        if (!tokenStream.peekTokenSameLine(&maybeFunction)) {
          return errorResult();
        }

        if (maybeFunction == TokenKind::Function) {
          error(JSMSG_FORBIDDEN_AS_STATEMENT, "async function declarations");
          return errorResult();
        }

        // Otherwise this |async| begins an ExpressionStatement or is a
        // label name.
      }

      // NOTE: It's unfortunately allowed to have a label named 'let' in
      //       non-strict code.  💯
      if (next == TokenKind::Colon) {
        return labeledStatement(yieldHandling);
      }

      return expressionStatement(yieldHandling);
    }

    case TokenKind::New:
      return expressionStatement(yieldHandling, PredictInvoked);

    // IfStatement[?Yield, ?Return]
    case TokenKind::If:
      return ifStatement(yieldHandling);

    // BreakableStatement[?Yield, ?Return]
    //
    // BreakableStatement[Yield, Return]:
    //   IterationStatement[?Yield, ?Return]
    //   SwitchStatement[?Yield, ?Return]
    case TokenKind::Do:
      return doWhileStatement(yieldHandling);

    case TokenKind::While:
      return whileStatement(yieldHandling);

    case TokenKind::For:
      return forStatement(yieldHandling);

    case TokenKind::Switch:
      return switchStatement(yieldHandling);

    // ContinueStatement[?Yield]
    case TokenKind::Continue:
      return continueStatement(yieldHandling);

    // BreakStatement[?Yield]
    case TokenKind::Break:
      return breakStatement(yieldHandling);

    // [+Return] ReturnStatement[?Yield]
    case TokenKind::Return:
      // The Return parameter is only used here, and the effect is easily
      // detected this way, so don't bother passing around an extra parameter
      // everywhere.
      if (!pc_->allowReturn()) {
        error(JSMSG_BAD_RETURN_OR_YIELD, "return");
        return errorResult();
      }
      return returnStatement(yieldHandling);

    // WithStatement[?Yield, ?Return]
    case TokenKind::With:
      return withStatement(yieldHandling);

    // LabelledStatement[?Yield, ?Return]
    // This is really handled by default and TokenKind::Yield cases above.

    // ThrowStatement[?Yield]
    case TokenKind::Throw:
      return throwStatement(yieldHandling);

    // TryStatement[?Yield, ?Return]
    case TokenKind::Try:
      return tryStatement(yieldHandling);

    // DebuggerStatement
    case TokenKind::Debugger:
      return debuggerStatement();

    // |function| is forbidden by lookahead restriction (unless as child
    // statement of |if| or |else|, but Parser::consequentOrAlternative
    // handles that).
    case TokenKind::Function:
      error(JSMSG_FORBIDDEN_AS_STATEMENT, "function declarations");
      return errorResult();

    // |class| is also forbidden by lookahead restriction.
    case TokenKind::Class:
      error(JSMSG_FORBIDDEN_AS_STATEMENT, "classes");
      return errorResult();

    // ImportDeclaration (only inside modules)
    case TokenKind::Import:
      return importDeclarationOrImportExpr(yieldHandling);

    // ExportDeclaration (only inside modules)
    case TokenKind::Export:
      return exportDeclaration();

      // Miscellaneous error cases arguably better caught here than elsewhere.

    case TokenKind::Catch:
      error(JSMSG_CATCH_WITHOUT_TRY);
      return errorResult();

    case TokenKind::Finally:
      error(JSMSG_FINALLY_WITHOUT_TRY);
      return errorResult();

      // NOTE: default case handled in the ExpressionStatement section.
  }
}

template <class ParseHandler, typename Unit>
typename ParseHandler::NodeResult
GeneralParser<ParseHandler, Unit>::statementListItem(
    YieldHandling yieldHandling, bool canHaveDirectives /* = false */) {
  MOZ_ASSERT(checkOptionsCalled_);

  AutoCheckRecursionLimit recursion(this->fc_);
  if (!recursion.check(this->fc_)) {
    return errorResult();
  }

  TokenKind tt;
  if (!tokenStream.getToken(&tt, TokenStream::SlashIsRegExp)) {
    return errorResult();
  }

  switch (tt) {
    // BlockStatement[?Yield, ?Return]
    case TokenKind::LeftCurly:
      return blockStatement(yieldHandling);

    // VariableStatement[?Yield]
    case TokenKind::Var:
      return variableStatement(yieldHandling);

    // EmptyStatement
    case TokenKind::Semi:
      return handler_.newEmptyStatement(pos());

    // ExpressionStatement[?Yield].
    //
    // These should probably be handled by a single ExpressionStatement
    // function in a default, not split up this way.
    case TokenKind::String:
      if (!canHaveDirectives &&
          anyChars.currentToken().atom() ==
              TaggedParserAtomIndex::WellKnown::use_asm_()) {
        if (!warning(JSMSG_USE_ASM_DIRECTIVE_FAIL)) {
          return errorResult();
        }
      }
      return expressionStatement(yieldHandling);

    case TokenKind::Yield: {
      // Don't use a ternary operator here due to obscure linker issues
      // around using static consts in the arms of a ternary.
      Modifier modifier;
      if (yieldExpressionsSupported()) {
        modifier = TokenStream::SlashIsRegExp;
      } else {
        modifier = TokenStream::SlashIsDiv;
      }

      TokenKind next;
      if (!tokenStream.peekToken(&next, modifier)) {
        return errorResult();
      }

      if (next == TokenKind::Colon) {
        return labeledStatement(yieldHandling);
      }

      return expressionStatement(yieldHandling);
    }

    default: {
      // If we encounter an await in a module, and the module is not marked
      // as async, mark the module as async.
      if (tt == TokenKind::Await && !pc_->isAsync()) {
        if (pc_->atModuleTopLevel()) {
          if (!options().topLevelAwait) {
            error(JSMSG_TOP_LEVEL_AWAIT_NOT_SUPPORTED);
            return errorResult();
          }
          pc_->sc()->asModuleContext()->setIsAsync();
          MOZ_ASSERT(pc_->isAsync());
        }
      }

      if (tt == TokenKind::Await && pc_->isAsync()) {
#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
        // Try finding evidence of a AwaitUsingDeclaration the syntax for which
        // would be:
        //   await [no LineTerminator here] using [no LineTerminator here]
        //     identifier

        TokenKind nextTokUsing = TokenKind::Eof;
        // Scan with regex modifier because when its await expression, `/`
        // should be treated as a regexp.
        if (!tokenStream.peekTokenSameLine(&nextTokUsing,
                                           TokenStream::SlashIsRegExp)) {
          return errorResult();
        }

        if (nextTokUsing == TokenKind::Using &&
            this->pc_->isUsingSyntaxAllowed()) {
          tokenStream.consumeKnownToken(nextTokUsing,
                                        TokenStream::SlashIsRegExp);
          TokenKind nextTokIdentifier = TokenKind::Eof;
          // Here we can use the Div modifier because if the next token is using
          // then a `/` as the next token can only be considered a division.
          if (!tokenStream.peekTokenSameLine(&nextTokIdentifier)) {
            return errorResult();
          }
          if (TokenKindIsPossibleIdentifier(nextTokIdentifier)) {
            return lexicalDeclaration(yieldHandling,
                                      DeclarationKind::AwaitUsing);
          }
          anyChars.ungetToken();  // put back using.
        }
#endif
        return expressionStatement(yieldHandling);
      }

      if (!TokenKindIsPossibleIdentifier(tt)) {
        return expressionStatement(yieldHandling);
      }

      TokenKind next;
      if (!tokenStream.peekToken(&next)) {
        return errorResult();
      }

      if (tt == TokenKind::Let && nextTokenContinuesLetDeclaration(next)) {
        return lexicalDeclaration(yieldHandling, DeclarationKind::Let);
      }

      if (tt == TokenKind::Async) {
        TokenKind nextSameLine = TokenKind::Eof;
        if (!tokenStream.peekTokenSameLine(&nextSameLine)) {
          return errorResult();
        }
        if (nextSameLine == TokenKind::Function) {
          uint32_t toStringStart = pos().begin;
          tokenStream.consumeKnownToken(TokenKind::Function);
          return functionStmt(toStringStart, yieldHandling, NameRequired,
                              FunctionAsyncKind::AsyncFunction);
        }
      }

      if (next == TokenKind::Colon) {
        return labeledStatement(yieldHandling);
      }

      return expressionStatement(yieldHandling);
    }

    case TokenKind::New:
      return expressionStatement(yieldHandling, PredictInvoked);

    // IfStatement[?Yield, ?Return]
    case TokenKind::If:
      return ifStatement(yieldHandling);

    // BreakableStatement[?Yield, ?Return]
    //
    // BreakableStatement[Yield, Return]:
    //   IterationStatement[?Yield, ?Return]
    //   SwitchStatement[?Yield, ?Return]
    case TokenKind::Do:
      return doWhileStatement(yieldHandling);

    case TokenKind::While:
      return whileStatement(yieldHandling);

    case TokenKind::For:
      return forStatement(yieldHandling);

    case TokenKind::Switch:
      return switchStatement(yieldHandling);

    // ContinueStatement[?Yield]
    case TokenKind::Continue:
      return continueStatement(yieldHandling);

    // BreakStatement[?Yield]
    case TokenKind::Break:
      return breakStatement(yieldHandling);

    // [+Return] ReturnStatement[?Yield]
    case TokenKind::Return:
      // The Return parameter is only used here, and the effect is easily
      // detected this way, so don't bother passing around an extra parameter
      // everywhere.
      if (!pc_->allowReturn()) {
        error(JSMSG_BAD_RETURN_OR_YIELD, "return");
        return errorResult();
      }
      return returnStatement(yieldHandling);

    // WithStatement[?Yield, ?Return]
    case TokenKind::With:
      return withStatement(yieldHandling);

    // LabelledStatement[?Yield, ?Return]
    // This is really handled by default and TokenKind::Yield cases above.

    // ThrowStatement[?Yield]
    case TokenKind::Throw:
      return throwStatement(yieldHandling);

    // TryStatement[?Yield, ?Return]
    case TokenKind::Try:
      return tryStatement(yieldHandling);

    // DebuggerStatement
    case TokenKind::Debugger:
      return debuggerStatement();

    // Declaration[Yield]:

    //   HoistableDeclaration[?Yield, ~Default]
    case TokenKind::Function:
      return functionStmt(pos().begin, yieldHandling, NameRequired);

      //   DecoratorList[?Yield, ?Await] opt ClassDeclaration[?Yield, ~Default]
#ifdef ENABLE_DECORATORS
    case TokenKind::At:
      return classDefinition(yieldHandling, ClassStatement, NameRequired);
#endif

    case TokenKind::Class:
      return classDefinition(yieldHandling, ClassStatement, NameRequired);

    //   LexicalDeclaration[In, ?Yield]
    //     LetOrConst BindingList[?In, ?Yield]
    case TokenKind::Const:
      // [In] is the default behavior, because for-loops specially parse
      // their heads to handle |in| in this situation.
      return lexicalDeclaration(yieldHandling, DeclarationKind::Const);

#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
    case TokenKind::Using: {
      TokenKind nextTok = TokenKind::Eol;
      if (!tokenStream.peekTokenSameLine(&nextTok)) {
        return errorResult();
      }
      if (!TokenKindIsPossibleIdentifier(nextTok) ||
          !this->pc_->isUsingSyntaxAllowed()) {
        if (!tokenStream.peekToken(&nextTok)) {
          return errorResult();
        }
        // labelled statement could be like using\n:\nexpr
        if (nextTok == TokenKind::Colon) {
          return labeledStatement(yieldHandling);
        }
        return expressionStatement(yieldHandling);
      }
      return lexicalDeclaration(yieldHandling, DeclarationKind::Using);
    }
#endif

    // ImportDeclaration (only inside modules)
    case TokenKind::Import:
      return importDeclarationOrImportExpr(yieldHandling);

    // ExportDeclaration (only inside modules)
    case TokenKind::Export:
      return exportDeclaration();

      // Miscellaneous error cases arguably better caught here than elsewhere.

    case TokenKind::Catch:
      error(JSMSG_CATCH_WITHOUT_TRY);
      return errorResult();

    case TokenKind::Finally:
      error(JSMSG_FINALLY_WITHOUT_TRY);
      return errorResult();

      // NOTE: default case handled in the ExpressionStatement section.
  }
}

template <class ParseHandler, typename Unit>
typename ParseHandler::NodeResult GeneralParser<ParseHandler, Unit>::expr(
    InHandling inHandling, YieldHandling yieldHandling,
    TripledotHandling tripledotHandling,
    PossibleError* possibleError /* = nullptr */,
    InvokedPrediction invoked /* = PredictUninvoked */) {
  Node pn;
  MOZ_TRY_VAR(pn, assignExpr(inHandling, yieldHandling, tripledotHandling,
                             possibleError, invoked));

  bool matched;
  if (!tokenStream.matchToken(&matched, TokenKind::Comma,
                              TokenStream::SlashIsRegExp)) {
    return errorResult();
  }
  if (!matched) {
    return pn;
  }

  ListNodeType seq;
  MOZ_TRY_VAR(seq, handler_.newCommaExpressionList(pn));
  while (true) {
    // Trailing comma before the closing parenthesis is valid in an arrow
    // function parameters list: `(a, b, ) => body`. Check if we are
    // directly under CoverParenthesizedExpressionAndArrowParameterList,
    // and the next two tokens are closing parenthesis and arrow. If all
    // are present allow the trailing comma.
    if (tripledotHandling == TripledotAllowed) {
      TokenKind tt;
      if (!tokenStream.peekToken(&tt, TokenStream::SlashIsRegExp)) {
        return errorResult();
      }

      if (tt == TokenKind::RightParen) {
        tokenStream.consumeKnownToken(TokenKind::RightParen,
                                      TokenStream::SlashIsRegExp);

        if (!tokenStream.peekToken(&tt)) {
          return errorResult();
        }
        if (tt != TokenKind::Arrow) {
          error(JSMSG_UNEXPECTED_TOKEN, "expression",
                TokenKindToDesc(TokenKind::RightParen));
          return errorResult();
        }

        anyChars.ungetToken();  // put back right paren
        break;
      }
    }

    // Additional calls to assignExpr should not reuse the possibleError
    // which had been passed into the function. Otherwise we would lose
    // information needed to determine whether or not we're dealing with
    // a non-recoverable situation.
    PossibleError possibleErrorInner(*this);
    MOZ_TRY_VAR(pn, assignExpr(inHandling, yieldHandling, tripledotHandling,
                               &possibleErrorInner));

    if (!possibleError) {
      // Report any pending expression error.
      if (!possibleErrorInner.checkForExpressionError()) {
        return errorResult();
      }
    } else {
      possibleErrorInner.transferErrorsTo(possibleError);
    }

    handler_.addList(seq, pn);

    if (!tokenStream.matchToken(&matched, TokenKind::Comma,
                                TokenStream::SlashIsRegExp)) {
      return errorResult();
    }
    if (!matched) {
      break;
    }
  }
  return seq;
}

static ParseNodeKind BinaryOpTokenKindToParseNodeKind(TokenKind tok) {
  MOZ_ASSERT(TokenKindIsBinaryOp(tok));
  return ParseNodeKind(size_t(ParseNodeKind::BinOpFirst) +
                       (size_t(tok) - size_t(TokenKind::BinOpFirst)));
}

// This list must be kept in the same order in several places:
//   - The binary operators in ParseNode.h ,
//   - the binary operators in TokenKind.h
//   - the JSOp code list in BytecodeEmitter.cpp
static const int PrecedenceTable[] = {
    1,  /* ParseNodeKind::Coalesce */
    2,  /* ParseNodeKind::Or */
    3,  /* ParseNodeKind::And */
    4,  /* ParseNodeKind::BitOr */
    5,  /* ParseNodeKind::BitXor */
    6,  /* ParseNodeKind::BitAnd */
    7,  /* ParseNodeKind::StrictEq */
    7,  /* ParseNodeKind::Eq */
    7,  /* ParseNodeKind::StrictNe */
    7,  /* ParseNodeKind::Ne */
    8,  /* ParseNodeKind::Lt */
    8,  /* ParseNodeKind::Le */
    8,  /* ParseNodeKind::Gt */
    8,  /* ParseNodeKind::Ge */
    8,  /* ParseNodeKind::InstanceOf */
    8,  /* ParseNodeKind::In */
    8,  /* ParseNodeKind::PrivateIn */
    9,  /* ParseNodeKind::Lsh */
    9,  /* ParseNodeKind::Rsh */
    9,  /* ParseNodeKind::Ursh */
    10, /* ParseNodeKind::Add */
    10, /* ParseNodeKind::Sub */
    11, /* ParseNodeKind::Star */
    11, /* ParseNodeKind::Div */
    11, /* ParseNodeKind::Mod */
    12  /* ParseNodeKind::Pow */
};

static const int PRECEDENCE_CLASSES = 12;

static int Precedence(ParseNodeKind pnk) {
  // Everything binds tighter than ParseNodeKind::Limit, because we want
  // to reduce all nodes to a single node when we reach a token that is not
  // another binary operator.
  if (pnk == ParseNodeKind::Limit) {
    return 0;
  }

  MOZ_ASSERT(pnk >= ParseNodeKind::BinOpFirst);
  MOZ_ASSERT(pnk <= ParseNodeKind::BinOpLast);
  return PrecedenceTable[size_t(pnk) - size_t(ParseNodeKind::BinOpFirst)];
}

enum class EnforcedParentheses : uint8_t { CoalesceExpr, AndOrExpr, None };

template <class ParseHandler, typename Unit>
MOZ_ALWAYS_INLINE typename ParseHandler::NodeResult
GeneralParser<ParseHandler, Unit>::orExpr(InHandling inHandling,
                                          YieldHandling yieldHandling,
                                          TripledotHandling tripledotHandling,
                                          PossibleError* possibleError,
                                          InvokedPrediction invoked) {
  // Shift-reduce parser for the binary operator part of the JS expression
  // syntax.

  // Conceptually there's just one stack, a stack of pairs (lhs, op).
  // It's implemented using two separate arrays, though.
  Node nodeStack[PRECEDENCE_CLASSES];
  ParseNodeKind kindStack[PRECEDENCE_CLASSES];
  int depth = 0;
  Node pn;
  EnforcedParentheses unparenthesizedExpression = EnforcedParentheses::None;
  for (;;) {
    MOZ_TRY_VAR(
        pn, unaryExpr(yieldHandling, tripledotHandling, possibleError, invoked,
                      PrivateNameHandling::PrivateNameAllowed));

    // If a binary operator follows, consume it and compute the
    // corresponding operator.
    TokenKind tok;
    if (!tokenStream.getToken(&tok)) {
      return errorResult();
    }

    // Ensure that if we have a private name lhs we are legally constructing a
    // `#x in obj` expessions:
    if (handler_.isPrivateName(pn)) {
      if (tok != TokenKind::In || inHandling != InAllowed) {
        error(JSMSG_ILLEGAL_PRIVATE_NAME);
        return errorResult();
      }
    }

    ParseNodeKind pnk;
    if (tok == TokenKind::In ? inHandling == InAllowed
                             : TokenKindIsBinaryOp(tok)) {
      // We're definitely not in a destructuring context, so report any
      // pending expression error now.
      if (possibleError && !possibleError->checkForExpressionError()) {
        return errorResult();
      }

      bool isErgonomicBrandCheck = false;
      switch (tok) {
        // Report an error for unary expressions on the LHS of **.
        case TokenKind::Pow:
          if (handler_.isUnparenthesizedUnaryExpression(pn)) {
            error(JSMSG_BAD_POW_LEFTSIDE);
            return errorResult();
          }
          break;

        case TokenKind::Or:
        case TokenKind::And:
          // In the case that the `??` is on the left hand side of the
          // expression: Disallow Mixing of ?? and other logical operators (||
          // and &&) unless one expression is parenthesized
          if (unparenthesizedExpression == EnforcedParentheses::CoalesceExpr) {
            error(JSMSG_BAD_COALESCE_MIXING);
            return errorResult();
          }
          // If we have not detected a mixing error at this point, record that
          // we have an unparenthesized expression, in case we have one later.
          unparenthesizedExpression = EnforcedParentheses::AndOrExpr;
          break;

        case TokenKind::Coalesce:
          if (unparenthesizedExpression == EnforcedParentheses::AndOrExpr) {
            error(JSMSG_BAD_COALESCE_MIXING);
            return errorResult();
          }
          // If we have not detected a mixing error at this point, record that
          // we have an unparenthesized expression, in case we have one later.
          unparenthesizedExpression = EnforcedParentheses::CoalesceExpr;
          break;

        case TokenKind::In:
          // if the LHS is a private name, and the operator is In,
          // ensure we're construcing an ergonomic brand check of
          // '#x in y', rather than having a higher precedence operator
          // like + cause a different reduction, such as
          // 1 + #x in y.
          if (handler_.isPrivateName(pn)) {
            if (depth > 0 && Precedence(kindStack[depth - 1]) >=
                                 Precedence(ParseNodeKind::InExpr)) {
              error(JSMSG_INVALID_PRIVATE_NAME_PRECEDENCE);
              return errorResult();
            }

            isErgonomicBrandCheck = true;
          }
          break;

        default:
          // do nothing in other cases
          break;
      }

      if (isErgonomicBrandCheck) {
        pnk = ParseNodeKind::PrivateInExpr;
      } else {
        pnk = BinaryOpTokenKindToParseNodeKind(tok);
      }

    } else {
      tok = TokenKind::Eof;
      pnk = ParseNodeKind::Limit;
    }

    // From this point on, destructuring defaults are definitely an error.
    possibleError = nullptr;

    // If pnk has precedence less than or equal to another operator on the
    // stack, reduce. This combines nodes on the stack until we form the
    // actual lhs of pnk.
    //
    // The >= in this condition works because it is appendOrCreateList's
    // job to decide if the operator in question is left- or
    // right-associative, and build the corresponding tree.
    while (depth > 0 && Precedence(kindStack[depth - 1]) >= Precedence(pnk)) {
      depth--;
      ParseNodeKind combiningPnk = kindStack[depth];
      MOZ_TRY_VAR(pn, handler_.appendOrCreateList(combiningPnk,
                                                  nodeStack[depth], pn, pc_));
    }

    if (pnk == ParseNodeKind::Limit) {
      break;
    }

    nodeStack[depth] = pn;
    kindStack[depth] = pnk;
    depth++;
    MOZ_ASSERT(depth <= PRECEDENCE_CLASSES);
  }

  anyChars.ungetToken();

  // Had the next token been a Div, we would have consumed it. So there's no
  // ambiguity if we later (after ASI) re-get this token with SlashIsRegExp.
  anyChars.allowGettingNextTokenWithSlashIsRegExp();

  MOZ_ASSERT(depth == 0);
  return pn;
}

template <class ParseHandler, typename Unit>
MOZ_ALWAYS_INLINE typename ParseHandler::NodeResult
GeneralParser<ParseHandler, Unit>::condExpr(InHandling inHandling,
                                            YieldHandling yieldHandling,
                                            TripledotHandling tripledotHandling,
                                            PossibleError* possibleError,
                                            InvokedPrediction invoked) {
  Node condition;
  MOZ_TRY_VAR(condition, orExpr(inHandling, yieldHandling, tripledotHandling,
                                possibleError, invoked));

  bool matched;
  if (!tokenStream.matchToken(&matched, TokenKind::Hook,
                              TokenStream::SlashIsInvalid)) {
    return errorResult();
  }
  if (!matched) {
    return condition;
  }

  Node thenExpr;
  MOZ_TRY_VAR(thenExpr,
              assignExpr(InAllowed, yieldHandling, TripledotProhibited));

  if (!mustMatchToken(TokenKind::Colon, JSMSG_COLON_IN_COND)) {
    return errorResult();
  }

  Node elseExpr;
  MOZ_TRY_VAR(elseExpr,
              assignExpr(inHandling, yieldHandling, TripledotProhibited));

  return handler_.newConditional(condition, thenExpr, elseExpr);
}

template <class ParseHandler, typename Unit>
typename ParseHandler::NodeResult GeneralParser<ParseHandler, Unit>::assignExpr(
    InHandling inHandling, YieldHandling yieldHandling,
    TripledotHandling tripledotHandling,
    PossibleError* possibleError /* = nullptr */,
    InvokedPrediction invoked /* = PredictUninvoked */) {
  AutoCheckRecursionLimit recursion(this->fc_);
  if (!recursion.check(this->fc_)) {
    return errorResult();
  }

  // It's very common at this point to have a "detectably simple" expression,
  // i.e. a name/number/string token followed by one of the following tokens
  // that obviously isn't part of an expression: , ; : ) ] }
  //
  // (In Parsemark this happens 81.4% of the time;  in code with large
  // numeric arrays, such as some Kraken benchmarks, it happens more often.)
  //
  // In such cases, we can avoid the full expression parsing route through
  // assignExpr(), condExpr(), orExpr(), unaryExpr(), memberExpr(), and
  // primaryExpr().

  TokenKind firstToken;
  if (!tokenStream.getToken(&firstToken, TokenStream::SlashIsRegExp)) {
    return errorResult();
  }

  TokenPos exprPos = pos();

  bool endsExpr;

  // This only handles identifiers that *never* have special meaning anywhere
  // in the language.  Contextual keywords, reserved words in strict mode,
  // and other hard cases are handled outside this fast path.
  if (firstToken == TokenKind::Name) {
    if (!tokenStream.nextTokenEndsExpr(&endsExpr)) {
      return errorResult();
    }
    if (endsExpr) {
      TaggedParserAtomIndex name = identifierReference(yieldHandling);
      if (!name) {
        return errorResult();
      }

      return identifierReference(name);
    }
  }

  if (firstToken == TokenKind::Number) {
    if (!tokenStream.nextTokenEndsExpr(&endsExpr)) {
      return errorResult();
    }
    if (endsExpr) {
      return newNumber(anyChars.currentToken());
    }
  }

  if (firstToken == TokenKind::String) {
    if (!tokenStream.nextTokenEndsExpr(&endsExpr)) {
      return errorResult();
    }
    if (endsExpr) {
      return stringLiteral();
    }
  }

  if (firstToken == TokenKind::Yield && yieldExpressionsSupported()) {
    return yieldExpression(inHandling);
  }

  bool maybeAsyncArrow = false;
  if (firstToken == TokenKind::Async) {
    TokenKind nextSameLine = TokenKind::Eof;
    if (!tokenStream.peekTokenSameLine(&nextSameLine)) {
      return errorResult();
    }

    if (TokenKindIsPossibleIdentifier(nextSameLine)) {
      maybeAsyncArrow = true;
    }
  }

  anyChars.ungetToken();

  // Save the tokenizer state in case we find an arrow function and have to
  // rewind.
  Position start(tokenStream);
  auto ghostToken = this->compilationState_.getPosition();

  PossibleError possibleErrorInner(*this);
  Node lhs;
  TokenKind tokenAfterLHS;
  bool isArrow;
  if (maybeAsyncArrow) {
    tokenStream.consumeKnownToken(TokenKind::Async, TokenStream::SlashIsRegExp);

    TokenKind tokenAfterAsync;
    if (!tokenStream.getToken(&tokenAfterAsync)) {
      return errorResult();
    }
    MOZ_ASSERT(TokenKindIsPossibleIdentifier(tokenAfterAsync));

    // Check yield validity here.
    TaggedParserAtomIndex name = bindingIdentifier(yieldHandling);
    if (!name) {
      return errorResult();
    }

    if (!tokenStream.peekToken(&tokenAfterLHS, TokenStream::SlashIsRegExp)) {
      return errorResult();
    }

    isArrow = tokenAfterLHS == TokenKind::Arrow;

    // |async [no LineTerminator] of| without being followed by => is only
    // possible in for-await-of loops, e.g. |for await (async of [])|. Pretend
    // the |async| token was parsed an identifier reference and then proceed
    // with the rest of this function.
    if (!isArrow) {
      anyChars.ungetToken();  // unget the binding identifier

      // The next token is guaranteed to never be a Div (, because it's an
      // identifier), so it's okay to re-get the token with SlashIsRegExp.
      anyChars.allowGettingNextTokenWithSlashIsRegExp();

      TaggedParserAtomIndex asyncName = identifierReference(yieldHandling);
      if (!asyncName) {
        return errorResult();
      }

      MOZ_TRY_VAR(lhs, identifierReference(asyncName));
    }
  } else {
    MOZ_TRY_VAR(lhs, condExpr(inHandling, yieldHandling, tripledotHandling,
                              &possibleErrorInner, invoked));

    // Use SlashIsRegExp here because the ConditionalExpression parsed above
    // could be the entirety of this AssignmentExpression, and then ASI
    // permits this token to be a regular expression.
    if (!tokenStream.peekToken(&tokenAfterLHS, TokenStream::SlashIsRegExp)) {
      return errorResult();
    }

    isArrow = tokenAfterLHS == TokenKind::Arrow;
  }

  if (isArrow) {
    // Rewind to reparse as an arrow function.
    //
    // Note: We do not call CompilationState::rewind here because parsing
    // during delazification will see the same rewind and need the same sequence
    // of inner functions to skip over.
    // Instead, we mark inner functions as "ghost".
    //
    // See GHOST_FUNCTION in FunctionFlags.h for more details.
    tokenStream.rewind(start);
    this->compilationState_.markGhost(ghostToken);

    TokenKind next;
    if (!tokenStream.getToken(&next, TokenStream::SlashIsRegExp)) {
      return errorResult();
    }
    TokenPos startPos = pos();
    uint32_t toStringStart = startPos.begin;
    anyChars.ungetToken();

    FunctionAsyncKind asyncKind = FunctionAsyncKind::SyncFunction;

    if (next == TokenKind::Async) {
      tokenStream.consumeKnownToken(next, TokenStream::SlashIsRegExp);

      TokenKind nextSameLine = TokenKind::Eof;
      if (!tokenStream.peekTokenSameLine(&nextSameLine)) {
        return errorResult();
      }

      // The AsyncArrowFunction production are
      //   async [no LineTerminator here] AsyncArrowBindingIdentifier ...
      //   async [no LineTerminator here] ArrowFormalParameters ...
      if (TokenKindIsPossibleIdentifier(nextSameLine) ||
          nextSameLine == TokenKind::LeftParen) {
        asyncKind = FunctionAsyncKind::AsyncFunction;
      } else {
        anyChars.ungetToken();
      }
    }

    FunctionSyntaxKind syntaxKind = FunctionSyntaxKind::Arrow;
    FunctionNodeType funNode;
    MOZ_TRY_VAR(funNode, handler_.newFunction(syntaxKind, startPos));

    return functionDefinition(funNode, toStringStart, inHandling, yieldHandling,
                              TaggedParserAtomIndex::null(), syntaxKind,
                              GeneratorKind::NotGenerator, asyncKind);
  }

  MOZ_ALWAYS_TRUE(
      tokenStream.getToken(&tokenAfterLHS, TokenStream::SlashIsRegExp));

  ParseNodeKind kind;
  switch (tokenAfterLHS) {
    case TokenKind::Assign:
      kind = ParseNodeKind::AssignExpr;
      break;
    case TokenKind::AddAssign:
      kind = ParseNodeKind::AddAssignExpr;
      break;
    case TokenKind::SubAssign:
      kind = ParseNodeKind::SubAssignExpr;
      break;
    case TokenKind::CoalesceAssign:
      kind = ParseNodeKind::CoalesceAssignExpr;
      break;
    case TokenKind::OrAssign:
      kind = ParseNodeKind::OrAssignExpr;
      break;
    case TokenKind::AndAssign:
      kind = ParseNodeKind::AndAssignExpr;
      break;
    case TokenKind::BitOrAssign:
      kind = ParseNodeKind::BitOrAssignExpr;
      break;
    case TokenKind::BitXorAssign:
      kind = ParseNodeKind::BitXorAssignExpr;
      break;
    case TokenKind::BitAndAssign:
      kind = ParseNodeKind::BitAndAssignExpr;
      break;
    case TokenKind::LshAssign:
      kind = ParseNodeKind::LshAssignExpr;
      break;
    case TokenKind::RshAssign:
      kind = ParseNodeKind::RshAssignExpr;
      break;
    case TokenKind::UrshAssign:
      kind = ParseNodeKind::UrshAssignExpr;
      break;
    case TokenKind::MulAssign:
      kind = ParseNodeKind::MulAssignExpr;
      break;
    case TokenKind::DivAssign:
      kind = ParseNodeKind::DivAssignExpr;
      break;
    case TokenKind::ModAssign:
      kind = ParseNodeKind::ModAssignExpr;
      break;
    case TokenKind::PowAssign:
      kind = ParseNodeKind::PowAssignExpr;
      break;

    default:
      MOZ_ASSERT(!anyChars.isCurrentTokenAssignment());
      if (!possibleError) {
        if (!possibleErrorInner.checkForExpressionError()) {
          return errorResult();
        }
      } else {
        possibleErrorInner.transferErrorsTo(possibleError);
      }

      anyChars.ungetToken();
      return lhs;
  }

  // Verify the left-hand side expression doesn't have a forbidden form.
  if (handler_.isUnparenthesizedDestructuringPattern(lhs)) {
    if (kind != ParseNodeKind::AssignExpr) {
      error(JSMSG_BAD_DESTRUCT_ASS);
      return errorResult();
    }

    if (!possibleErrorInner.checkForDestructuringErrorOrWarning()) {
      return errorResult();
    }
  } else if (handler_.isName(lhs)) {
    if (const char* chars = nameIsArgumentsOrEval(lhs)) {
      // |chars| is "arguments" or "eval" here.
      if (!strictModeErrorAt(exprPos.begin, JSMSG_BAD_STRICT_ASSIGN, chars)) {
        return errorResult();
      }
    }
  } else if (handler_.isArgumentsLength(lhs)) {
    pc_->sc()->setIneligibleForArgumentsLength();
  } else if (handler_.isPropertyOrPrivateMemberAccess(lhs)) {
    // Permitted: no additional testing/fixup needed.
  } else if (handler_.isFunctionCall(lhs)) {
    // We don't have to worry about backward compatibility issues with the new
    // compound assignment operators, so we always throw here. Also that way we
    // don't have to worry if |f() &&= expr| should always throw an error or
    // only if |f()| returns true.
    if (kind == ParseNodeKind::CoalesceAssignExpr ||
        kind == ParseNodeKind::OrAssignExpr ||
        kind == ParseNodeKind::AndAssignExpr) {
      errorAt(exprPos.begin, JSMSG_BAD_LEFTSIDE_OF_ASS);
      return errorResult();
    }

    if (!strictModeErrorAt(exprPos.begin, JSMSG_BAD_LEFTSIDE_OF_ASS)) {
      return errorResult();
    }

    if (possibleError) {
      possibleError->setPendingDestructuringErrorAt(exprPos,
                                                    JSMSG_BAD_DESTRUCT_TARGET);
    }
  } else {
    errorAt(exprPos.begin, JSMSG_BAD_LEFTSIDE_OF_ASS);
    return errorResult();
  }

  if (!possibleErrorInner.checkForExpressionError()) {
    return errorResult();
  }

  Node rhs;
  MOZ_TRY_VAR(rhs, assignExpr(inHandling, yieldHandling, TripledotProhibited));

  return handler_.newAssignment(kind, lhs, rhs);
}

template <class ParseHandler>
const char* PerHandlerParser<ParseHandler>::nameIsArgumentsOrEval(Node node) {
  MOZ_ASSERT(handler_.isName(node),
             "must only call this function on known names");

  if (handler_.isEvalName(node)) {
    return "eval";
  }
  if (handler_.isArgumentsName(node)) {
    return "arguments";
  }
  return nullptr;
}

template <class ParseHandler, typename Unit>
bool GeneralParser<ParseHandler, Unit>::checkIncDecOperand(
    Node operand, uint32_t operandOffset) {
  if (handler_.isName(operand)) {
    if (const char* chars = nameIsArgumentsOrEval(operand)) {
      if (!strictModeErrorAt(operandOffset, JSMSG_BAD_STRICT_ASSIGN, chars)) {
        return false;
      }
    }
  } else if (handler_.isArgumentsLength(operand)) {
    pc_->sc()->setIneligibleForArgumentsLength();
  } else if (handler_.isPropertyOrPrivateMemberAccess(operand)) {
    // Permitted: no additional testing/fixup needed.
  } else if (handler_.isFunctionCall(operand)) {
    // Assignment to function calls is forbidden in ES6.  We're still
    // somewhat concerned about sites using this in dead code, so forbid it
    // only in strict mode code.
    if (!strictModeErrorAt(operandOffset, JSMSG_BAD_INCOP_OPERAND)) {
      return false;
    }
  } else {
    errorAt(operandOffset, JSMSG_BAD_INCOP_OPERAND);
    return false;
  }
  return true;
}

template <class ParseHandler, typename Unit>
typename ParseHandler::UnaryNodeResult
GeneralParser<ParseHandler, Unit>::unaryOpExpr(YieldHandling yieldHandling,
                                               ParseNodeKind kind,
                                               uint32_t begin) {
  Node kid;
  MOZ_TRY_VAR(kid, unaryExpr(yieldHandling, TripledotProhibited));
  return handler_.newUnary(kind, begin, kid);
}

template <class ParseHandler, typename Unit>
typename ParseHandler::NodeResult
GeneralParser<ParseHandler, Unit>::optionalExpr(
    YieldHandling yieldHandling, TripledotHandling tripledotHandling,
    TokenKind tt, PossibleError* possibleError /* = nullptr */,
    InvokedPrediction invoked /* = PredictUninvoked */) {
  AutoCheckRecursionLimit recursion(this->fc_);
  if (!recursion.check(this->fc_)) {
    return errorResult();
  }

  uint32_t begin = pos().begin;

  Node lhs;
  MOZ_TRY_VAR(lhs,
              memberExpr(yieldHandling, tripledotHandling, tt,
                         /* allowCallSyntax = */ true, possibleError, invoked));

  if (!tokenStream.peekToken(&tt, TokenStream::SlashIsDiv)) {
    return errorResult();
  }

  if (tt != TokenKind::OptionalChain) {
    return lhs;
  }

  while (true) {
    if (!tokenStream.getToken(&tt)) {
      return errorResult();
    }

    if (tt == TokenKind::Eof) {
      anyChars.ungetToken();
      break;
    }

    Node nextMember;
    if (tt == TokenKind::OptionalChain) {
      if (!tokenStream.getToken(&tt)) {
        return errorResult();
      }
      if (TokenKindIsPossibleIdentifierName(tt)) {
        MOZ_TRY_VAR(nextMember,
                    memberPropertyAccess(lhs, OptionalKind::Optional));
      } else if (tt == TokenKind::PrivateName) {
        MOZ_TRY_VAR(nextMember,
                    memberPrivateAccess(lhs, OptionalKind::Optional));
      } else if (tt == TokenKind::LeftBracket) {
        MOZ_TRY_VAR(nextMember, memberElemAccess(lhs, yieldHandling,
                                                 OptionalKind::Optional));
      } else if (tt == TokenKind::LeftParen) {
        MOZ_TRY_VAR(nextMember,
                    memberCall(tt, lhs, yieldHandling, possibleError,
                               OptionalKind::Optional));
      } else {
        error(JSMSG_NAME_AFTER_DOT);
        return errorResult();
      }
    } else if (tt == TokenKind::Dot) {
      if (!tokenStream.getToken(&tt)) {
        return errorResult();
      }
      if (TokenKindIsPossibleIdentifierName(tt)) {
        MOZ_TRY_VAR(nextMember, memberPropertyAccess(lhs));
      } else if (tt == TokenKind::PrivateName) {
        MOZ_TRY_VAR(nextMember, memberPrivateAccess(lhs));
      } else {
        error(JSMSG_NAME_AFTER_DOT);
        return errorResult();
      }
    } else if (tt == TokenKind::LeftBracket) {
      MOZ_TRY_VAR(nextMember, memberElemAccess(lhs, yieldHandling));
    } else if (tt == TokenKind::LeftParen) {
      MOZ_TRY_VAR(nextMember,
                  memberCall(tt, lhs, yieldHandling, possibleError));
    } else if (tt == TokenKind::TemplateHead ||
               tt == TokenKind::NoSubsTemplate) {
      error(JSMSG_BAD_OPTIONAL_TEMPLATE);
      return errorResult();
    } else {
      anyChars.ungetToken();
      break;
    }

    MOZ_ASSERT(nextMember);
    lhs = nextMember;
  }

  return handler_.newOptionalChain(begin, lhs);
}

template <class ParseHandler, typename Unit>
typename ParseHandler::NodeResult GeneralParser<ParseHandler, Unit>::unaryExpr(
    YieldHandling yieldHandling, TripledotHandling tripledotHandling,
    PossibleError* possibleError /* = nullptr */,
    InvokedPrediction invoked /* = PredictUninvoked */,
    PrivateNameHandling privateNameHandling /* = PrivateNameProhibited */) {
  AutoCheckRecursionLimit recursion(this->fc_);
  if (!recursion.check(this->fc_)) {
    return errorResult();
  }

  TokenKind tt;
  if (!tokenStream.getToken(&tt, TokenStream::SlashIsRegExp)) {
    return errorResult();
  }
  uint32_t begin = pos().begin;
  switch (tt) {
    case TokenKind::Void:
      return unaryOpExpr(yieldHandling, ParseNodeKind::VoidExpr, begin);
    case TokenKind::Not:
      return unaryOpExpr(yieldHandling, ParseNodeKind::NotExpr, begin);
    case TokenKind::BitNot:
      return unaryOpExpr(yieldHandling, ParseNodeKind::BitNotExpr, begin);
    case TokenKind::Add:
      return unaryOpExpr(yieldHandling, ParseNodeKind::PosExpr, begin);
    case TokenKind::Sub:
      return unaryOpExpr(yieldHandling, ParseNodeKind::NegExpr, begin);

    case TokenKind::TypeOf: {
      // The |typeof| operator is specially parsed to distinguish its
      // application to a name, from its application to a non-name
      // expression:
      //
      //   // Looks up the name, doesn't find it and so evaluates to
      //   // "undefined".
      //   assertEq(typeof nonExistentName, "undefined");
      //
      //   // Evaluates expression, triggering a runtime ReferenceError for
      //   // the undefined name.
      //   typeof (1, nonExistentName);
      Node kid;
      MOZ_TRY_VAR(kid, unaryExpr(yieldHandling, TripledotProhibited));

      return handler_.newTypeof(begin, kid);
    }

    case TokenKind::Inc:
    case TokenKind::Dec: {
      TokenKind tt2;
      if (!tokenStream.getToken(&tt2, TokenStream::SlashIsRegExp)) {
        return errorResult();
      }

      uint32_t operandOffset = pos().begin;
      Node operand;
      MOZ_TRY_VAR(operand,
                  optionalExpr(yieldHandling, TripledotProhibited, tt2));
      if (!checkIncDecOperand(operand, operandOffset)) {
        return errorResult();
      }
      ParseNodeKind pnk = (tt == TokenKind::Inc)
                              ? ParseNodeKind::PreIncrementExpr
                              : ParseNodeKind::PreDecrementExpr;
      return handler_.newUpdate(pnk, begin, operand);
    }
    case TokenKind::PrivateName: {
      if (privateNameHandling == PrivateNameHandling::PrivateNameAllowed) {
        TaggedParserAtomIndex field = anyChars.currentName();
        return privateNameReference(field);
      }
      error(JSMSG_INVALID_PRIVATE_NAME_IN_UNARY_EXPR);
      return errorResult();
    }

    case TokenKind::Delete: {
      uint32_t exprOffset;
      if (!tokenStream.peekOffset(&exprOffset, TokenStream::SlashIsRegExp)) {
        return errorResult();
      }

      Node expr;
      MOZ_TRY_VAR(expr, unaryExpr(yieldHandling, TripledotProhibited));

      // Per spec, deleting most unary expressions is valid -- it simply
      // returns true -- except for two cases:
      // 1. `var x; ...; delete x` is a syntax error in strict mode.
      // 2. Private fields cannot be deleted.
      if (handler_.isName(expr)) {
        if (!strictModeErrorAt(exprOffset, JSMSG_DEPRECATED_DELETE_OPERAND)) {
          return errorResult();
        }

        pc_->sc()->setBindingsAccessedDynamically();
      }

      if (handler_.isPrivateMemberAccess(expr)) {
        errorAt(exprOffset, JSMSG_PRIVATE_DELETE);
        return errorResult();
      }

      return handler_.newDelete(begin, expr);
    }
    case TokenKind::Await: {
      // If we encounter an await in a module, mark it as async.
      if (!pc_->isAsync() && pc_->sc()->isModule()) {
        if (!options().topLevelAwait) {
          error(JSMSG_TOP_LEVEL_AWAIT_NOT_SUPPORTED);
          return errorResult();
        }
        pc_->sc()->asModuleContext()->setIsAsync();
        MOZ_ASSERT(pc_->isAsync());
      }

      if (pc_->isAsync()) {
        if (inParametersOfAsyncFunction()) {
          error(JSMSG_AWAIT_IN_PARAMETER);
          return errorResult();
        }
        Node kid;
        MOZ_TRY_VAR(kid, unaryExpr(yieldHandling, tripledotHandling,
                                   possibleError, invoked));
        pc_->lastAwaitOffset = begin;
        return handler_.newAwaitExpression(begin, kid);
      }
    }

      [[fallthrough]];

    default: {
      Node expr;
      MOZ_TRY_VAR(expr, optionalExpr(yieldHandling, tripledotHandling, tt,
                                     possibleError, invoked));

      /* Don't look across a newline boundary for a postfix incop. */
      if (!tokenStream.peekTokenSameLine(&tt)) {
        return errorResult();
      }

      if (tt != TokenKind::Inc && tt != TokenKind::Dec) {
        return expr;
      }

      tokenStream.consumeKnownToken(tt);
      if (!checkIncDecOperand(expr, begin)) {
        return errorResult();
      }

      ParseNodeKind pnk = (tt == TokenKind::Inc)
                              ? ParseNodeKind::PostIncrementExpr
                              : ParseNodeKind::PostDecrementExpr;
      return handler_.newUpdate(pnk, begin, expr);
    }
  }
}

template <class ParseHandler, typename Unit>
typename ParseHandler::NodeResult
GeneralParser<ParseHandler, Unit>::assignExprWithoutYieldOrAwait(
    YieldHandling yieldHandling) {
  uint32_t startYieldOffset = pc_->lastYieldOffset;
  uint32_t startAwaitOffset = pc_->lastAwaitOffset;

  Node res;
  MOZ_TRY_VAR(res, assignExpr(InAllowed, yieldHandling, TripledotProhibited));

  if (pc_->lastYieldOffset != startYieldOffset) {
    errorAt(pc_->lastYieldOffset, JSMSG_YIELD_IN_PARAMETER);
    return errorResult();
  }
  if (pc_->lastAwaitOffset != startAwaitOffset) {
    errorAt(pc_->lastAwaitOffset, JSMSG_AWAIT_IN_PARAMETER);
    return errorResult();
  }
  return res;
}

template <class ParseHandler, typename Unit>
typename ParseHandler::ListNodeResult
GeneralParser<ParseHandler, Unit>::argumentList(
    YieldHandling yieldHandling, bool* isSpread,
    PossibleError* possibleError /* = nullptr */) {
  ListNodeType argsList;
  MOZ_TRY_VAR(argsList, handler_.newArguments(pos()));

  bool matched;
  if (!tokenStream.matchToken(&matched, TokenKind::RightParen,
                              TokenStream::SlashIsRegExp)) {
    return errorResult();
  }
  if (matched) {
    handler_.setEndPosition(argsList, pos().end);
    return argsList;
  }

  while (true) {
    bool spread = false;
    uint32_t begin = 0;
    if (!tokenStream.matchToken(&matched, TokenKind::TripleDot,
                                TokenStream::SlashIsRegExp)) {
      return errorResult();
    }
    if (matched) {
      spread = true;
      begin = pos().begin;
      *isSpread = true;
    }

    Node argNode;
    MOZ_TRY_VAR(argNode, assignExpr(InAllowed, yieldHandling,
                                    TripledotProhibited, possibleError));
    if (spread) {
      MOZ_TRY_VAR(argNode, handler_.newSpread(begin, argNode));
    }

    handler_.addList(argsList, argNode);

    bool matched;
    if (!tokenStream.matchToken(&matched, TokenKind::Comma,
                                TokenStream::SlashIsRegExp)) {
      return errorResult();
    }
    if (!matched) {
      break;
    }

    TokenKind tt;
    if (!tokenStream.peekToken(&tt, TokenStream::SlashIsRegExp)) {
      return errorResult();
    }
    if (tt == TokenKind::RightParen) {
      break;
    }
  }

  if (!mustMatchToken(TokenKind::RightParen, JSMSG_PAREN_AFTER_ARGS)) {
    return errorResult();
  }

  handler_.setEndPosition(argsList, pos().end);
  return argsList;
}

bool ParserBase::checkAndMarkSuperScope() {
  if (!pc_->sc()->allowSuperProperty()) {
    return false;
  }

  pc_->setSuperScopeNeedsHomeObject();
  return true;
}

template <class ParseHandler, typename Unit>
bool GeneralParser<ParseHandler, Unit>::computeErrorMetadata(
    ErrorMetadata* err, const ErrorReportMixin::ErrorOffset& offset) const {
  if (offset.is<ErrorReportMixin::Current>()) {
    return tokenStream.computeErrorMetadata(err, AsVariant(pos().begin));
  }
  return tokenStream.computeErrorMetadata(err, offset);
}

template <class ParseHandler, typename Unit>
typename ParseHandler::NodeResult GeneralParser<ParseHandler, Unit>::memberExpr(
    YieldHandling yieldHandling, TripledotHandling tripledotHandling,
    TokenKind tt, bool allowCallSyntax, PossibleError* possibleError,
    InvokedPrediction invoked) {
  MOZ_ASSERT(anyChars.isCurrentTokenType(tt));

  Node lhs;

  AutoCheckRecursionLimit recursion(this->fc_);
  if (!recursion.check(this->fc_)) {
    return errorResult();
  }

  /* Check for new expression first. */
  if (tt == TokenKind::New) {
    uint32_t newBegin = pos().begin;
    // Make sure this wasn't a |new.target| in disguise.
    NewTargetNodeType newTarget;
    if (!tryNewTarget(&newTarget)) {
      return errorResult();
    }
    if (newTarget) {
      lhs = newTarget;
    } else {
      // Gotten by tryNewTarget
      tt = anyChars.currentToken().type;
      Node ctorExpr;
      MOZ_TRY_VAR(ctorExpr,
                  memberExpr(yieldHandling, TripledotProhibited, tt,
                             /* allowCallSyntax = */ false,
                             /* possibleError = */ nullptr, PredictInvoked));

      // If we have encountered an optional chain, in the form of `new
      // ClassName?.()` then we need to throw, as this is disallowed by the
      // spec.
      bool optionalToken;
      if (!tokenStream.matchToken(&optionalToken, TokenKind::OptionalChain)) {
        return errorResult();
      }
      if (optionalToken) {
        errorAt(newBegin, JSMSG_BAD_NEW_OPTIONAL);
        return errorResult();
      }

      bool matched;
      if (!tokenStream.matchToken(&matched, TokenKind::LeftParen)) {
        return errorResult();
      }

      bool isSpread = false;
      ListNodeType args;
      if (matched) {
        MOZ_TRY_VAR(args, argumentList(yieldHandling, &isSpread));
      } else {
        MOZ_TRY_VAR(args, handler_.newArguments(pos()));
      }

      if (!args) {
        return errorResult();
      }

      MOZ_TRY_VAR(
          lhs, handler_.newNewExpression(newBegin, ctorExpr, args, isSpread));
    }
  } else if (tt == TokenKind::Super) {
    NameNodeType thisName;
    MOZ_TRY_VAR(thisName, newThisName());
    MOZ_TRY_VAR(lhs, handler_.newSuperBase(thisName, pos()));
  } else if (tt == TokenKind::Import) {
    MOZ_TRY_VAR(lhs, importExpr(yieldHandling, allowCallSyntax));
  } else {
    MOZ_TRY_VAR(lhs, primaryExpr(yieldHandling, tripledotHandling, tt,
                                 possibleError, invoked));
  }

  MOZ_ASSERT_IF(handler_.isSuperBase(lhs),
                anyChars.isCurrentTokenType(TokenKind::Super));

  while (true) {
    if (!tokenStream.getToken(&tt)) {
      return errorResult();
    }
    if (tt == TokenKind::Eof) {
      anyChars.ungetToken();
      break;
    }

    Node nextMember;
    if (tt == TokenKind::Dot) {
      if (!tokenStream.getToken(&tt)) {
        return errorResult();
      }

      if (TokenKindIsPossibleIdentifierName(tt)) {
        MOZ_TRY_VAR(nextMember, memberPropertyAccess(lhs));
      } else if (tt == TokenKind::PrivateName) {
        MOZ_TRY_VAR(nextMember, memberPrivateAccess(lhs));
      } else {
        error(JSMSG_NAME_AFTER_DOT);
        return errorResult();
      }
    } else if (tt == TokenKind::LeftBracket) {
      MOZ_TRY_VAR(nextMember, memberElemAccess(lhs, yieldHandling));
    } else if ((allowCallSyntax && tt == TokenKind::LeftParen) ||
               tt == TokenKind::TemplateHead ||
               tt == TokenKind::NoSubsTemplate) {
      if (handler_.isSuperBase(lhs)) {
        if (!pc_->sc()->allowSuperCall()) {
          error(JSMSG_BAD_SUPERCALL);
          return errorResult();
        }

        if (tt != TokenKind::LeftParen) {
          error(JSMSG_BAD_SUPER);
          return errorResult();
        }

        MOZ_TRY_VAR(nextMember, memberSuperCall(lhs, yieldHandling));

        if (!noteUsedName(
                TaggedParserAtomIndex::WellKnown::dot_initializers_())) {
          return errorResult();
        }
#ifdef ENABLE_DECORATORS
        if (!noteUsedName(TaggedParserAtomIndex::WellKnown::
                              dot_instanceExtraInitializers_())) {
          return null();
        }
#endif
      } else {
        MOZ_TRY_VAR(nextMember,
                    memberCall(tt, lhs, yieldHandling, possibleError));
      }
    } else {
      anyChars.ungetToken();
      if (handler_.isSuperBase(lhs)) {
        break;
      }
      return lhs;
    }

    lhs = nextMember;
  }

  if (handler_.isSuperBase(lhs)) {
    error(JSMSG_BAD_SUPER);
    return errorResult();
  }

  return lhs;
}

template <class ParseHandler, typename Unit>
typename ParseHandler::NodeResult
GeneralParser<ParseHandler, Unit>::decoratorExpr(YieldHandling yieldHandling,
                                                 TokenKind tt) {
  MOZ_ASSERT(anyChars.isCurrentTokenType(tt));

  AutoCheckRecursionLimit recursion(this->fc_);
  if (!recursion.check(this->fc_)) {
    return errorResult();
  }

  if (tt == TokenKind::LeftParen) {
    // DecoratorParenthesizedExpression
    Node expr;
    MOZ_TRY_VAR(expr, exprInParens(InAllowed, yieldHandling, TripledotAllowed,
                                   /* possibleError*/ nullptr));
    if (!mustMatchToken(TokenKind::RightParen, JSMSG_PAREN_AFTER_DECORATOR)) {
      return errorResult();
    }

    return handler_.parenthesize(expr);
  }

  if (!TokenKindIsPossibleIdentifier(tt)) {
    error(JSMSG_DECORATOR_NAME_EXPECTED);
    return errorResult();
  }

  TaggedParserAtomIndex name = identifierReference(yieldHandling);
  if (!name) {
    return errorResult();
  }

  Node lhs;
  MOZ_TRY_VAR(lhs, identifierReference(name));

  while (true) {
    if (!tokenStream.getToken(&tt)) {
      return errorResult();
    }
    if (tt == TokenKind::Eof) {
      anyChars.ungetToken();
      break;
    }

    Node nextMember;
    if (tt == TokenKind::Dot) {
      if (!tokenStream.getToken(&tt)) {
        return errorResult();
      }

      if (TokenKindIsPossibleIdentifierName(tt)) {
        MOZ_TRY_VAR(nextMember, memberPropertyAccess(lhs));
      } else if (tt == TokenKind::PrivateName) {
        MOZ_TRY_VAR(nextMember, memberPrivateAccess(lhs));
      } else {
        error(JSMSG_NAME_AFTER_DOT);
        return errorResult();
      }
    } else if (tt == TokenKind::LeftParen) {
      MOZ_TRY_VAR(nextMember, memberCall(tt, lhs, yieldHandling,
                                         /* possibleError */ nullptr));
      lhs = nextMember;
      // This is a `DecoratorCallExpression` and it's defined at the top level
      // of `Decorator`, no other `DecoratorMemberExpression` is allowed to
      // follow after the arguments.
      break;
    } else {
      anyChars.ungetToken();
      break;
    }

    lhs = nextMember;
  }

  return lhs;
}

template <class ParseHandler>
inline typename ParseHandler::NameNodeResult
PerHandlerParser<ParseHandler>::newName(TaggedParserAtomIndex name) {
  return newName(name, pos());
}

template <class ParseHandler>
inline typename ParseHandler::NameNodeResult
PerHandlerParser<ParseHandler>::newName(TaggedParserAtomIndex name,
                                        TokenPos pos) {
  if (name == TaggedParserAtomIndex::WellKnown::arguments()) {
    this->pc_->numberOfArgumentsNames++;
  }
  return handler_.newName(name, pos);
}

template <class ParseHandler>
inline typename ParseHandler::NameNodeResult
PerHandlerParser<ParseHandler>::newPrivateName(TaggedParserAtomIndex name) {
  return handler_.newPrivateName(name, pos());
}

template <class ParseHandler, typename Unit>
typename ParseHandler::NodeResult
GeneralParser<ParseHandler, Unit>::memberPropertyAccess(
    Node lhs, OptionalKind optionalKind /* = OptionalKind::NonOptional */) {
  MOZ_ASSERT(TokenKindIsPossibleIdentifierName(anyChars.currentToken().type) ||
             anyChars.currentToken().type == TokenKind::PrivateName);
  TaggedParserAtomIndex field = anyChars.currentName();
  if (handler_.isSuperBase(lhs) && !checkAndMarkSuperScope()) {
    error(JSMSG_BAD_SUPERPROP, "property");
    return errorResult();
  }

  NameNodeType name;
  MOZ_TRY_VAR(name, handler_.newPropertyName(field, pos()));

  if (optionalKind == OptionalKind::Optional) {
    MOZ_ASSERT(!handler_.isSuperBase(lhs));
    return handler_.newOptionalPropertyAccess(lhs, name);
  }

  if (handler_.isArgumentsName(lhs) && handler_.isLengthName(name)) {
    MOZ_ASSERT(pc_->numberOfArgumentsNames > 0);
    pc_->numberOfArgumentsNames--;
    // Currently when resuming Generators don't get their argument length set
    // in the interpreter frame (see InterpreterStack::resumeGeneratorCallFrame,
    // and its call to initCallFrame).
    if (pc_->isGeneratorOrAsync()) {
      pc_->sc()->setIneligibleForArgumentsLength();
    }
    return handler_.newArgumentsLength(lhs, name);
  }

  return handler_.newPropertyAccess(lhs, name);
}

template <class ParseHandler, typename Unit>
typename ParseHandler::NodeResult
GeneralParser<ParseHandler, Unit>::memberPrivateAccess(
    Node lhs, OptionalKind optionalKind /* = OptionalKind::NonOptional */) {
  MOZ_ASSERT(anyChars.currentToken().type == TokenKind::PrivateName);

  TaggedParserAtomIndex field = anyChars.currentName();
  // Cannot access private fields on super.
  if (handler_.isSuperBase(lhs)) {
    error(JSMSG_BAD_SUPERPRIVATE);
    return errorResult();
  }

  NameNodeType privateName;
  MOZ_TRY_VAR(privateName, privateNameReference(field));

  if (optionalKind == OptionalKind::Optional) {
    MOZ_ASSERT(!handler_.isSuperBase(lhs));
    return handler_.newOptionalPrivateMemberAccess(lhs, privateName, pos().end);
  }
  return handler_.newPrivateMemberAccess(lhs, privateName, pos().end);
}

template <class ParseHandler, typename Unit>
typename ParseHandler::NodeResult
GeneralParser<ParseHandler, Unit>::memberElemAccess(
    Node lhs, YieldHandling yieldHandling,
    OptionalKind optionalKind /* = OptionalKind::NonOptional */) {
  MOZ_ASSERT(anyChars.currentToken().type == TokenKind::LeftBracket);
  Node propExpr;
  MOZ_TRY_VAR(propExpr, expr(InAllowed, yieldHandling, TripledotProhibited));

  if (!mustMatchToken(TokenKind::RightBracket, JSMSG_BRACKET_IN_INDEX)) {
    return errorResult();
  }

  if (handler_.isSuperBase(lhs) && !checkAndMarkSuperScope()) {
    error(JSMSG_BAD_SUPERPROP, "member");
    return errorResult();
  }
  if (optionalKind == OptionalKind::Optional) {
    MOZ_ASSERT(!handler_.isSuperBase(lhs));
    return handler_.newOptionalPropertyByValue(lhs, propExpr, pos().end);
  }
  return handler_.newPropertyByValue(lhs, propExpr, pos().end);
}

template <class ParseHandler, typename Unit>
typename ParseHandler::NodeResult
GeneralParser<ParseHandler, Unit>::memberSuperCall(
    Node lhs, YieldHandling yieldHandling) {
  MOZ_ASSERT(anyChars.currentToken().type == TokenKind::LeftParen);
  // Despite the fact that it's impossible to have |super()| in a
  // generator, we still inherit the yieldHandling of the
  // memberExpression, per spec. Curious.
  bool isSpread = false;
  ListNodeType args;
  MOZ_TRY_VAR(args, argumentList(yieldHandling, &isSpread));

  CallNodeType superCall;
  MOZ_TRY_VAR(superCall, handler_.newSuperCall(lhs, args, isSpread));

  // |super()| implicitly reads |new.target|.
  if (!noteUsedName(TaggedParserAtomIndex::WellKnown::dot_newTarget_())) {
    return errorResult();
  }

  NameNodeType thisName;
  MOZ_TRY_VAR(thisName, newThisName());

  return handler_.newSetThis(thisName, superCall);
}

template <class ParseHandler, typename Unit>
typename ParseHandler::NodeResult GeneralParser<ParseHandler, Unit>::memberCall(
    TokenKind tt, Node lhs, YieldHandling yieldHandling,
    PossibleError* possibleError /* = nullptr */,
    OptionalKind optionalKind /* = OptionalKind::NonOptional */) {
  if (options().selfHostingMode &&
      (handler_.isPropertyOrPrivateMemberAccess(lhs) ||
       handler_.isOptionalPropertyOrPrivateMemberAccess(lhs))) {
    error(JSMSG_SELFHOSTED_METHOD_CALL);
    return errorResult();
  }

  MOZ_ASSERT(tt == TokenKind::LeftParen || tt == TokenKind::TemplateHead ||
                 tt == TokenKind::NoSubsTemplate,
             "Unexpected token kind for member call");

  JSOp op = JSOp::Call;
  bool maybeAsyncArrow = false;
  if (tt == TokenKind::LeftParen && optionalKind == OptionalKind::NonOptional) {
    if (handler_.isAsyncKeyword(lhs)) {
      // |async (| can be the start of an async arrow
      // function, so we need to defer reporting possible
      // errors from destructuring syntax. To give better
      // error messages, we only allow the AsyncArrowHead
      // part of the CoverCallExpressionAndAsyncArrowHead
      // syntax when the initial name is "async".
      maybeAsyncArrow = true;
    } else if (handler_.isEvalName(lhs)) {
      // Select the right Eval op and flag pc_ as having a
      // direct eval.
      op = pc_->sc()->strict() ? JSOp::StrictEval : JSOp::Eval;
      pc_->sc()->setBindingsAccessedDynamically();
      pc_->sc()->setHasDirectEval();

      // In non-strict mode code, direct calls to eval can
      // add variables to the call object.
      if (pc_->isFunctionBox() && !pc_->sc()->strict()) {
        pc_->functionBox()->setFunHasExtensibleScope();
      }

      // If we're in a method, mark the method as requiring
      // support for 'super', since direct eval code can use
      // it. (If we're not in a method, that's fine, so
      // ignore the return value.)
      checkAndMarkSuperScope();
    }
  }

  if (tt == TokenKind::LeftParen) {
    bool isSpread = false;
    PossibleError* asyncPossibleError =
        maybeAsyncArrow ? possibleError : nullptr;
    ListNodeType args;
    MOZ_TRY_VAR(args,
                argumentList(yieldHandling, &isSpread, asyncPossibleError));
    if (isSpread) {
      if (op == JSOp::Eval) {
        op = JSOp::SpreadEval;
      } else if (op == JSOp::StrictEval) {
        op = JSOp::StrictSpreadEval;
      } else {
        op = JSOp::SpreadCall;
      }
    }

    if (optionalKind == OptionalKind::Optional) {
      return handler_.newOptionalCall(lhs, args, op);
    }
    return handler_.newCall(lhs, args, op);
  }

  ListNodeType args;
  MOZ_TRY_VAR(args, handler_.newArguments(pos()));

  if (!taggedTemplate(yieldHandling, args, tt)) {
    return errorResult();
  }

  if (optionalKind == OptionalKind::Optional) {
    error(JSMSG_BAD_OPTIONAL_TEMPLATE);
    return errorResult();
  }

  return handler_.newTaggedTemplate(lhs, args, op);
}

template <class ParseHandler, typename Unit>
bool GeneralParser<ParseHandler, Unit>::checkLabelOrIdentifierReference(
    TaggedParserAtomIndex ident, uint32_t offset, YieldHandling yieldHandling,
    TokenKind hint /* = TokenKind::Limit */) {
  TokenKind tt;
  if (hint == TokenKind::Limit) {
    tt = ReservedWordTokenKind(ident);
  } else {
    // All non-reserved word kinds are folded into TokenKind::Limit in
    // ReservedWordTokenKind and the following code.
    if (hint == TokenKind::Name || hint == TokenKind::PrivateName) {
      hint = TokenKind::Limit;
    }
    MOZ_ASSERT(hint == ReservedWordTokenKind(ident),
               "hint doesn't match actual token kind");
    tt = hint;
  }

  if (!pc_->sc()->allowArguments() &&
      ident == TaggedParserAtomIndex::WellKnown::arguments()) {
    error(JSMSG_BAD_ARGUMENTS);
    return false;
  }

  if (tt == TokenKind::Limit) {
    // Either TokenKind::Name or TokenKind::PrivateName
    return true;
  }
  if (TokenKindIsContextualKeyword(tt)) {
    if (tt == TokenKind::Yield) {
      if (yieldHandling == YieldIsKeyword) {
        errorAt(offset, JSMSG_RESERVED_ID, "yield");
        return false;
      }
      if (pc_->sc()->strict()) {
        if (!strictModeErrorAt(offset, JSMSG_RESERVED_ID, "yield")) {
          return false;
        }
      }
      return true;
    }
    if (tt == TokenKind::Await) {
      if (awaitIsKeyword() || awaitIsDisallowed()) {
        errorAt(offset, JSMSG_RESERVED_ID, "await");
        return false;
      }
      return true;
    }
    if (pc_->sc()->strict()) {
      if (tt == TokenKind::Let) {
        if (!strictModeErrorAt(offset, JSMSG_RESERVED_ID, "let")) {
          return false;
        }
        return true;
      }
      if (tt == TokenKind::Static) {
        if (!strictModeErrorAt(offset, JSMSG_RESERVED_ID, "static")) {
          return false;
        }
        return true;
      }
    }
    return true;
  }
  if (TokenKindIsStrictReservedWord(tt)) {
    if (pc_->sc()->strict()) {
      if (!strictModeErrorAt(offset, JSMSG_RESERVED_ID,
                             ReservedWordToCharZ(tt))) {
        return false;
      }
    }
    return true;
  }
  if (TokenKindIsKeyword(tt) || TokenKindIsReservedWordLiteral(tt)) {
    errorAt(offset, JSMSG_INVALID_ID, ReservedWordToCharZ(tt));
    return false;
  }
  if (TokenKindIsFutureReservedWord(tt)) {
    errorAt(offset, JSMSG_RESERVED_ID, ReservedWordToCharZ(tt));
    return false;
  }
  MOZ_ASSERT_UNREACHABLE("Unexpected reserved word kind.");
  return false;
}

template <class ParseHandler, typename Unit>
bool GeneralParser<ParseHandler, Unit>::checkBindingIdentifier(
    TaggedParserAtomIndex ident, uint32_t offset, YieldHandling yieldHandling,
    TokenKind hint /* = TokenKind::Limit */) {
  if (pc_->sc()->strict()) {
    if (ident == TaggedParserAtomIndex::WellKnown::arguments()) {
      if (!strictModeErrorAt(offset, JSMSG_BAD_STRICT_ASSIGN, "arguments")) {
        return false;
      }
      return true;
    }

    if (ident == TaggedParserAtomIndex::WellKnown::eval()) {
      if (!strictModeErrorAt(offset, JSMSG_BAD_STRICT_ASSIGN, "eval")) {
        return false;
      }
      return true;
    }
  }

  return checkLabelOrIdentifierReference(ident, offset, yieldHandling, hint);
}

template <class ParseHandler, typename Unit>
TaggedParserAtomIndex
GeneralParser<ParseHandler, Unit>::labelOrIdentifierReference(
    YieldHandling yieldHandling) {
  // ES 2017 draft 12.1.1.
  //   StringValue of IdentifierName normalizes any Unicode escape sequences
  //   in IdentifierName hence such escapes cannot be used to write an
  //   Identifier whose code point sequence is the same as a ReservedWord.
  //
  // Use const ParserName* instead of TokenKind to reflect the normalization.

  // Unless the name contains escapes, we can reuse the current TokenKind
  // to determine if the name is a restricted identifier.
  TokenKind hint = !anyChars.currentNameHasEscapes(this->parserAtoms())
                       ? anyChars.currentToken().type
                       : TokenKind::Limit;
  TaggedParserAtomIndex ident = anyChars.currentName();
  if (!checkLabelOrIdentifierReference(ident, pos().begin, yieldHandling,
                                       hint)) {
    return TaggedParserAtomIndex::null();
  }
  return ident;
}

template <class ParseHandler, typename Unit>
TaggedParserAtomIndex GeneralParser<ParseHandler, Unit>::bindingIdentifier(
    YieldHandling yieldHandling) {
  TokenKind hint = !anyChars.currentNameHasEscapes(this->parserAtoms())
                       ? anyChars.currentToken().type
                       : TokenKind::Limit;
  TaggedParserAtomIndex ident = anyChars.currentName();
  if (!checkBindingIdentifier(ident, pos().begin, yieldHandling, hint)) {
    return TaggedParserAtomIndex::null();
  }
  return ident;
}

template <class ParseHandler>
typename ParseHandler::NameNodeResult
PerHandlerParser<ParseHandler>::identifierReference(
    TaggedParserAtomIndex name) {
  NameNodeType id;
  MOZ_TRY_VAR(id, newName(name));

  if (!noteUsedName(name)) {
    return errorResult();
  }

  return id;
}

template <class ParseHandler>
typename ParseHandler::NameNodeResult
PerHandlerParser<ParseHandler>::privateNameReference(
    TaggedParserAtomIndex name) {
  NameNodeType id;
  MOZ_TRY_VAR(id, newPrivateName(name));

  if (!noteUsedName(name, NameVisibility::Private, Some(pos()))) {
    return errorResult();
  }

  return id;
}

template <class ParseHandler>
typename ParseHandler::NameNodeResult
PerHandlerParser<ParseHandler>::stringLiteral() {
  return handler_.newStringLiteral(anyChars.currentToken().atom(), pos());
}

template <class ParseHandler>
typename ParseHandler::NodeResult
PerHandlerParser<ParseHandler>::noSubstitutionTaggedTemplate() {
  if (anyChars.hasInvalidTemplateEscape()) {
    anyChars.clearInvalidTemplateEscape();
    return handler_.newRawUndefinedLiteral(pos());
  }

  return handler_.newTemplateStringLiteral(anyChars.currentToken().atom(),
                                           pos());
}

template <class ParseHandler, typename Unit>
typename ParseHandler::NameNodeResult
GeneralParser<ParseHandler, Unit>::noSubstitutionUntaggedTemplate() {
  if (!tokenStream.checkForInvalidTemplateEscapeError()) {
    return errorResult();
  }

  return handler_.newTemplateStringLiteral(anyChars.currentToken().atom(),
                                           pos());
}

template <typename Unit>
FullParseHandler::RegExpLiteralResult
Parser<FullParseHandler, Unit>::newRegExp() {
  MOZ_ASSERT(!options().selfHostingMode);

  // Create the regexp and check its syntax.
  const auto& chars = tokenStream.getCharBuffer();
  mozilla::Range<const char16_t> range(chars.begin(), chars.length());
  RegExpFlags flags = anyChars.currentToken().regExpFlags();

  uint32_t offset = anyChars.currentToken().pos.begin;
  uint32_t line;
  JS::LimitedColumnNumberOneOrigin column;
  tokenStream.computeLineAndColumn(offset, &line, &column);

  if (!handler_.reuseRegexpSyntaxParse()) {
    // Verify that the Regexp will syntax parse when the time comes to
    // instantiate it. If we have already done a syntax parse, we can
    // skip this.
    if (!irregexp::CheckPatternSyntax(
            this->alloc_, this->fc_->stackLimit(), anyChars, range, flags,
            Some(line), Some(JS::ColumnNumberOneOrigin(column)))) {
      return errorResult();
    }
  }

  auto atom =
      this->parserAtoms().internChar16(fc_, chars.begin(), chars.length());
  if (!atom) {
    return errorResult();
  }
  // RegExp patterm must be atomized.
  this->parserAtoms().markUsedByStencil(atom, ParserAtom::Atomize::Yes);

  RegExpIndex index(this->compilationState_.regExpData.length());
  if (uint32_t(index) >= TaggedScriptThingIndex::IndexLimit) {
    ReportAllocationOverflow(fc_);
    return errorResult();
  }
  if (!this->compilationState_.regExpData.emplaceBack(atom, flags)) {
    js::ReportOutOfMemory(this->fc_);
    return errorResult();
  }

  return handler_.newRegExp(index, pos());
}

template <typename Unit>
SyntaxParseHandler::RegExpLiteralResult
Parser<SyntaxParseHandler, Unit>::newRegExp() {
  MOZ_ASSERT(!options().selfHostingMode);

  // Only check the regexp's syntax, but don't create a regexp object.
  const auto& chars = tokenStream.getCharBuffer();
  RegExpFlags flags = anyChars.currentToken().regExpFlags();

  uint32_t offset = anyChars.currentToken().pos.begin;
  uint32_t line;
  JS::LimitedColumnNumberOneOrigin column;
  tokenStream.computeLineAndColumn(offset, &line, &column);

  mozilla::Range<const char16_t> source(chars.begin(), chars.length());
  if (!irregexp::CheckPatternSyntax(this->alloc_, this->fc_->stackLimit(),
                                    anyChars, source, flags, Some(line),
                                    Some(JS::ColumnNumberOneOrigin(column)))) {
    return errorResult();
  }

  return handler_.newRegExp(SyntaxParseHandler::Node::NodeGeneric, pos());
}

template <class ParseHandler, typename Unit>
typename ParseHandler::RegExpLiteralResult
GeneralParser<ParseHandler, Unit>::newRegExp() {
  return asFinalParser()->newRegExp();
}

template <typename Unit>
FullParseHandler::BigIntLiteralResult
Parser<FullParseHandler, Unit>::newBigInt() {
  // The token's charBuffer contains the DecimalIntegerLiteral or
  // NonDecimalIntegerLiteral production, and as such does not include the
  // BigIntLiteralSuffix (the trailing "n").  Note that NonDecimalIntegerLiteral
  // productions start with 0[bBoOxX], indicating binary/octal/hex.
  const auto& chars = tokenStream.getCharBuffer();
  if (chars.length() > UINT32_MAX) {
    ReportAllocationOverflow(fc_);
    return errorResult();
  }

  BigIntIndex index(this->compilationState_.bigIntData.length());
  if (uint32_t(index) >= TaggedScriptThingIndex::IndexLimit) {
    ReportAllocationOverflow(fc_);
    return errorResult();
  }
  if (!this->compilationState_.bigIntData.emplaceBack()) {
    js::ReportOutOfMemory(this->fc_);
    return errorResult();
  }

  if (!this->compilationState_.bigIntData[index].init(
          this->fc_, this->stencilAlloc(), chars)) {
    return errorResult();
  }

  bool isZero = this->compilationState_.bigIntData[index].isZero();

  // Should the operations below fail, the buffer held by data will
  // be cleaned up by the CompilationState destructor.
  return handler_.newBigInt(index, isZero, pos());
}

template <typename Unit>
SyntaxParseHandler::BigIntLiteralResult
Parser<SyntaxParseHandler, Unit>::newBigInt() {
  // The tokenizer has already checked the syntax of the bigint.

  return handler_.newBigInt();
}

template <class ParseHandler, typename Unit>
typename ParseHandler::BigIntLiteralResult
GeneralParser<ParseHandler, Unit>::newBigInt() {
  return asFinalParser()->newBigInt();
}

// |exprPossibleError| is the PossibleError state within |expr|,
// |possibleError| is the surrounding PossibleError state.
template <class ParseHandler, typename Unit>
bool GeneralParser<ParseHandler, Unit>::checkDestructuringAssignmentTarget(
    Node expr, TokenPos exprPos, PossibleError* exprPossibleError,
    PossibleError* possibleError, TargetBehavior behavior) {
  // Report any pending expression error if we're definitely not in a
  // destructuring context or the possible destructuring target is a
  // property accessor.
  if (!possibleError || handler_.isPropertyOrPrivateMemberAccess(expr)) {
    return exprPossibleError->checkForExpressionError();
  }

  // |expr| may end up as a destructuring assignment target, so we need to
  // validate it's either a name or can be parsed as a nested destructuring
  // pattern. Property accessors are also valid assignment targets, but
  // those are already handled above.

  exprPossibleError->transferErrorsTo(possibleError);

  // Return early if a pending destructuring error is already present.
  if (possibleError->hasPendingDestructuringError()) {
    return true;
  }

  if (handler_.isName(expr)) {
    checkDestructuringAssignmentName(handler_.asNameNode(expr), exprPos,
                                     possibleError);
    return true;
  }

  if (handler_.isUnparenthesizedDestructuringPattern(expr)) {
    if (behavior == TargetBehavior::ForbidAssignmentPattern) {
      possibleError->setPendingDestructuringErrorAt(exprPos,
                                                    JSMSG_BAD_DESTRUCT_TARGET);
    }
    return true;
  }

  // Parentheses are forbidden around destructuring *patterns* (but allowed
  // around names). Use our nicer error message for parenthesized, nested
  // patterns if nested destructuring patterns are allowed.
  if (handler_.isParenthesizedDestructuringPattern(expr) &&
      behavior != TargetBehavior::ForbidAssignmentPattern) {
    possibleError->setPendingDestructuringErrorAt(exprPos,
                                                  JSMSG_BAD_DESTRUCT_PARENS);
  } else {
    possibleError->setPendingDestructuringErrorAt(exprPos,
                                                  JSMSG_BAD_DESTRUCT_TARGET);
  }

  return true;
}

template <class ParseHandler, typename Unit>
void GeneralParser<ParseHandler, Unit>::checkDestructuringAssignmentName(
    NameNodeType name, TokenPos namePos, PossibleError* possibleError) {
#ifdef DEBUG
  // GCC 8.0.1 crashes if this is a one-liner.
  bool isName = handler_.isName(name);
  MOZ_ASSERT(isName);
#endif

  // Return early if a pending destructuring error is already present.
  if (possibleError->hasPendingDestructuringError()) {
    return;
  }

  if (handler_.isArgumentsLength(name)) {
    pc_->sc()->setIneligibleForArgumentsLength();
  }

  if (pc_->sc()->strict()) {
    if (handler_.isArgumentsName(name)) {
      if (pc_->sc()->strict()) {
        possibleError->setPendingDestructuringErrorAt(
            namePos, JSMSG_BAD_STRICT_ASSIGN_ARGUMENTS);
      } else {
        possibleError->setPendingDestructuringWarningAt(
            namePos, JSMSG_BAD_STRICT_ASSIGN_ARGUMENTS);
      }
      return;
    }

    if (handler_.isEvalName(name)) {
      if (pc_->sc()->strict()) {
        possibleError->setPendingDestructuringErrorAt(
            namePos, JSMSG_BAD_STRICT_ASSIGN_EVAL);
      } else {
        possibleError->setPendingDestructuringWarningAt(
            namePos, JSMSG_BAD_STRICT_ASSIGN_EVAL);
      }
      return;
    }
  }
}

template <class ParseHandler, typename Unit>
bool GeneralParser<ParseHandler, Unit>::checkDestructuringAssignmentElement(
    Node expr, TokenPos exprPos, PossibleError* exprPossibleError,
    PossibleError* possibleError) {
  // ES2018 draft rev 0719f44aab93215ed9a626b2f45bd34f36916834
  // 12.15.5 Destructuring Assignment
  //
  // AssignmentElement[Yield, Await]:
  //   DestructuringAssignmentTarget[?Yield, ?Await]
  //   DestructuringAssignmentTarget[?Yield, ?Await] Initializer[+In,
  //                                                             ?Yield,
  //                                                             ?Await]

  // If |expr| is an assignment element with an initializer expression, its
  // destructuring assignment target was already validated in assignExpr().
  // Otherwise we need to check that |expr| is a valid destructuring target.
  if (handler_.isUnparenthesizedAssignment(expr)) {
    // Report any pending expression error if we're definitely not in a
    // destructuring context.
    if (!possibleError) {
      return exprPossibleError->checkForExpressionError();
    }

    exprPossibleError->transferErrorsTo(possibleError);
    return true;
  }
  return checkDestructuringAssignmentTarget(expr, exprPos, exprPossibleError,
                                            possibleError);
}

template <class ParseHandler, typename Unit>
typename ParseHandler::ListNodeResult
GeneralParser<ParseHandler, Unit>::arrayInitializer(
    YieldHandling yieldHandling, PossibleError* possibleError) {
  MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::LeftBracket));

  uint32_t begin = pos().begin;
  ListNodeType literal;
  MOZ_TRY_VAR(literal, handler_.newArrayLiteral(begin));

  TokenKind tt;
  if (!tokenStream.getToken(&tt, TokenStream::SlashIsRegExp)) {
    return errorResult();
  }

  if (tt == TokenKind::RightBracket) {
    /*
     * Mark empty arrays as non-constant, since we cannot easily
     * determine their type.
     */
    handler_.setListHasNonConstInitializer(literal);
  } else {
    anyChars.ungetToken();

    for (uint32_t index = 0;; index++) {
      if (index >= NativeObject::MAX_DENSE_ELEMENTS_COUNT) {
        error(JSMSG_ARRAY_INIT_TOO_BIG);
        return errorResult();
      }

      TokenKind tt;
      if (!tokenStream.peekToken(&tt, TokenStream::SlashIsRegExp)) {
        return errorResult();
      }
      if (tt == TokenKind::RightBracket) {
        break;
      }

      if (tt == TokenKind::Comma) {
        tokenStream.consumeKnownToken(TokenKind::Comma,
                                      TokenStream::SlashIsRegExp);
        if (!handler_.addElision(literal, pos())) {
          return errorResult();
        }
        continue;
      }

      if (tt == TokenKind::TripleDot) {
        tokenStream.consumeKnownToken(TokenKind::TripleDot,
                                      TokenStream::SlashIsRegExp);
        uint32_t begin = pos().begin;

        TokenPos innerPos;
        if (!tokenStream.peekTokenPos(&innerPos, TokenStream::SlashIsRegExp)) {
          return errorResult();
        }

        PossibleError possibleErrorInner(*this);
        Node inner;
        MOZ_TRY_VAR(inner,
                    assignExpr(InAllowed, yieldHandling, TripledotProhibited,
                               &possibleErrorInner));
        if (!checkDestructuringAssignmentTarget(
                inner, innerPos, &possibleErrorInner, possibleError)) {
          return errorResult();
        }

        if (!handler_.addSpreadElement(literal, begin, inner)) {
          return errorResult();
        }
      } else {
        TokenPos elementPos;
        if (!tokenStream.peekTokenPos(&elementPos,
                                      TokenStream::SlashIsRegExp)) {
          return errorResult();
        }

        PossibleError possibleErrorInner(*this);
        Node element;
        MOZ_TRY_VAR(element,
                    assignExpr(InAllowed, yieldHandling, TripledotProhibited,
                               &possibleErrorInner));
        if (!checkDestructuringAssignmentElement(
                element, elementPos, &possibleErrorInner, possibleError)) {
          return errorResult();
        }
        handler_.addArrayElement(literal, element);
      }

      bool matched;
      if (!tokenStream.matchToken(&matched, TokenKind::Comma,
                                  TokenStream::SlashIsRegExp)) {
        return errorResult();
      }
      if (!matched) {
        break;
      }

      if (tt == TokenKind::TripleDot && possibleError) {
        possibleError->setPendingDestructuringErrorAt(pos(),
                                                      JSMSG_REST_WITH_COMMA);
      }
    }

    if (!mustMatchToken(
            TokenKind::RightBracket, [this, begin](TokenKind actual) {
              this->reportMissingClosing(JSMSG_BRACKET_AFTER_LIST,
                                         JSMSG_BRACKET_OPENED, begin);
            })) {
      return errorResult();
    }
  }

  handler_.setEndPosition(literal, pos().end);
  return literal;
}

template <class ParseHandler, typename Unit>
typename ParseHandler::NodeResult
GeneralParser<ParseHandler, Unit>::propertyName(
    YieldHandling yieldHandling, PropertyNameContext propertyNameContext,
    const Maybe<DeclarationKind>& maybeDecl, ListNodeType propList,
    TaggedParserAtomIndex* propAtomOut) {
  // PropertyName[Yield, Await]:
  //   LiteralPropertyName
  //   ComputedPropertyName[?Yield, ?Await]
  //
  // LiteralPropertyName:
  //   IdentifierName
  //   StringLiteral
  //   NumericLiteral
  TokenKind ltok = anyChars.currentToken().type;

  *propAtomOut = TaggedParserAtomIndex::null();
  switch (ltok) {
    case TokenKind::Number: {
      auto numAtom = NumberToParserAtom(fc_, this->parserAtoms(),
                                        anyChars.currentToken().number());
      if (!numAtom) {
        return errorResult();
      }
      *propAtomOut = numAtom;
      return newNumber(anyChars.currentToken());
    }

    case TokenKind::BigInt: {
      Node biNode;
      MOZ_TRY_VAR(biNode, newBigInt());
      return handler_.newSyntheticComputedName(biNode, pos().begin, pos().end);
    }
    case TokenKind::String: {
      auto str = anyChars.currentToken().atom();
      *propAtomOut = str;
      uint32_t index;
      if (this->parserAtoms().isIndex(str, &index)) {
        return handler_.newNumber(index, NoDecimal, pos());
      }
      return stringLiteral();
    }

    case TokenKind::LeftBracket:
      return computedPropertyName(yieldHandling, maybeDecl, propertyNameContext,
                                  propList);

    case TokenKind::PrivateName: {
      if (propertyNameContext != PropertyNameContext::PropertyNameInClass) {
        error(JSMSG_ILLEGAL_PRIVATE_FIELD);
        return errorResult();
      }

      TaggedParserAtomIndex propName = anyChars.currentName();
      *propAtomOut = propName;
      return privateNameReference(propName);
    }

    default: {
      if (!TokenKindIsPossibleIdentifierName(ltok)) {
        error(JSMSG_UNEXPECTED_TOKEN, "property name", TokenKindToDesc(ltok));
        return errorResult();
      }

      TaggedParserAtomIndex name = anyChars.currentName();
      *propAtomOut = name;
      return handler_.newObjectLiteralPropertyName(name, pos());
    }
  }
}

// True if `kind` can be the first token of a PropertyName.
static bool TokenKindCanStartPropertyName(TokenKind tt) {
  return TokenKindIsPossibleIdentifierName(tt) || tt == TokenKind::String ||
         tt == TokenKind::Number || tt == TokenKind::LeftBracket ||
         tt == TokenKind::Mul || tt == TokenKind::BigInt ||
         tt == TokenKind::PrivateName;
}

template <class ParseHandler, typename Unit>
typename ParseHandler::NodeResult
GeneralParser<ParseHandler, Unit>::propertyOrMethodName(
    YieldHandling yieldHandling, PropertyNameContext propertyNameContext,
    const Maybe<DeclarationKind>& maybeDecl, ListNodeType propList,
    PropertyType* propType, TaggedParserAtomIndex* propAtomOut) {
  // We're parsing an object literal, class, or destructuring pattern;
  // propertyNameContext tells which one. This method parses any of the
  // following, storing the corresponding PropertyType in `*propType` to tell
  // the caller what we parsed:
  //
  //     async [no LineTerminator here] PropertyName
  //                            ==> PropertyType::AsyncMethod
  //     async [no LineTerminator here] * PropertyName
  //                            ==> PropertyType::AsyncGeneratorMethod
  //     * PropertyName         ==> PropertyType::GeneratorMethod
  //     get PropertyName       ==> PropertyType::Getter
  //     set PropertyName       ==> PropertyType::Setter
  //     accessor PropertyName  ==> PropertyType::FieldWithAccessor
  //     PropertyName :         ==> PropertyType::Normal
  //     PropertyName           ==> see below
  //
  // In the last case, where there's not a `:` token to consume, we peek at
  // (but don't consume) the next token to decide how to set `*propType`.
  //
  //     `,` or `}`             ==> PropertyType::Shorthand
  //     `(`                    ==> PropertyType::Method
  //     `=`, not in a class    ==> PropertyType::CoverInitializedName
  //     '=', in a class        ==> PropertyType::Field
  //     any token, in a class  ==> PropertyType::Field (ASI)
  //
  // The caller must check `*propType` and throw if whatever we parsed isn't
  // allowed here (for example, a getter in a destructuring pattern).
  //
  // This method does *not* match `static` (allowed in classes) or `...`
  // (allowed in object literals and patterns). The caller must take care of
  // those before calling this method.

  TokenKind ltok;
  if (!tokenStream.getToken(&ltok, TokenStream::SlashIsInvalid)) {
    return errorResult();
  }

  MOZ_ASSERT(ltok != TokenKind::RightCurly,
             "caller should have handled TokenKind::RightCurly");

  // Accept `async` and/or `*`, indicating an async or generator method;
  // or `get` or `set` or `accessor`, indicating an accessor.
  bool isGenerator = false;
  bool isAsync = false;
  bool isGetter = false;
  bool isSetter = false;
#ifdef ENABLE_DECORATORS
  bool hasAccessor = false;
#endif

  if (ltok == TokenKind::Async) {
    // `async` is also a PropertyName by itself (it's a conditional keyword),
    // so peek at the next token to see if we're really looking at a method.
    TokenKind tt = TokenKind::Eof;
    if (!tokenStream.peekTokenSameLine(&tt)) {
      return errorResult();
    }
    if (TokenKindCanStartPropertyName(tt)) {
      isAsync = true;
      tokenStream.consumeKnownToken(tt);
      ltok = tt;
    }
  }

  if (ltok == TokenKind::Mul) {
    isGenerator = true;
    if (!tokenStream.getToken(&ltok)) {
      return errorResult();
    }
  }

  if (!isAsync && !isGenerator &&
      (ltok == TokenKind::Get || ltok == TokenKind::Set)) {
    // We have parsed |get| or |set|. Look for an accessor property
    // name next.
    TokenKind tt;
    if (!tokenStream.peekToken(&tt)) {
      return errorResult();
    }
    if (TokenKindCanStartPropertyName(tt)) {
      tokenStream.consumeKnownToken(tt);
      isGetter = (ltok == TokenKind::Get);
      isSetter = (ltok == TokenKind::Set);
    }
  }

#ifdef ENABLE_DECORATORS
  if (!isGenerator && !isAsync && propertyNameContext == PropertyNameInClass &&
      ltok == TokenKind::Accessor) {
    MOZ_ASSERT(!isGetter && !isSetter);
    TokenKind tt;
    if (!tokenStream.peekTokenSameLine(&tt)) {
      return errorResult();
    }

    // The target rule is `accessor [no LineTerminator here]
    // ClassElementName[?Yield, ?Await] Initializer[+In, ?Yield, ?Await]opt`
    if (TokenKindCanStartPropertyName(tt)) {
      tokenStream.consumeKnownToken(tt);
      hasAccessor = true;
    }
  }
#endif

  Node propName;
  MOZ_TRY_VAR(propName, propertyName(yieldHandling, propertyNameContext,
                                     maybeDecl, propList, propAtomOut));

  // Grab the next token following the property/method name.
  // (If this isn't a colon, we're going to either put it back or throw.)
  TokenKind tt;
  if (!tokenStream.getToken(&tt)) {
    return errorResult();
  }

  if (tt == TokenKind::Colon) {
    if (isGenerator || isAsync || isGetter || isSetter
#ifdef ENABLE_DECORATORS
        || hasAccessor
#endif
    ) {
      error(JSMSG_BAD_PROP_ID);
      return errorResult();
    }
    *propType = PropertyType::Normal;
    return propName;
  }

  if (propertyNameContext != PropertyNameInClass &&
      TokenKindIsPossibleIdentifierName(ltok) &&
      (tt == TokenKind::Comma || tt == TokenKind::RightCurly ||
       tt == TokenKind::Assign)) {
#ifdef ENABLE_DECORATORS
    MOZ_ASSERT(!hasAccessor);
#endif
    if (isGenerator || isAsync || isGetter || isSetter) {
      error(JSMSG_BAD_PROP_ID);
      return errorResult();
    }

    anyChars.ungetToken();
    *propType = tt == TokenKind::Assign ? PropertyType::CoverInitializedName
                                        : PropertyType::Shorthand;
    return propName;
  }

  if (tt == TokenKind::LeftParen) {
    anyChars.ungetToken();

#ifdef ENABLE_RECORD_TUPLE
    if (propertyNameContext == PropertyNameInRecord) {
      // Record & Tuple proposal, section 7.1.1:
      // RecordPropertyDefinition doesn't cover methods
      error(JSMSG_BAD_PROP_ID);
      return errorResult();
    }
#endif

#ifdef ENABLE_DECORATORS
    if (hasAccessor) {
      error(JSMSG_BAD_PROP_ID);
      return errorResult();
    }
#endif

    if (isGenerator && isAsync) {
      *propType = PropertyType::AsyncGeneratorMethod;
    } else if (isGenerator) {
      *propType = PropertyType::GeneratorMethod;
    } else if (isAsync) {
      *propType = PropertyType::AsyncMethod;
    } else if (isGetter) {
      *propType = PropertyType::Getter;
    } else if (isSetter) {
      *propType = PropertyType::Setter;
    } else {
      *propType = PropertyType::Method;
    }
    return propName;
  }

  if (propertyNameContext == PropertyNameInClass) {
    if (isGenerator || isAsync || isGetter || isSetter) {
      error(JSMSG_BAD_PROP_ID);
      return errorResult();
    }
    anyChars.ungetToken();
#ifdef ENABLE_DECORATORS
    if (!hasAccessor) {
      *propType = PropertyType::Field;
    } else {
      *propType = PropertyType::FieldWithAccessor;
    }
#else
    *propType = PropertyType::Field;
#endif
    return propName;
  }

  error(JSMSG_COLON_AFTER_ID);
  return errorResult();
}

template <class ParseHandler, typename Unit>
typename ParseHandler::UnaryNodeResult
GeneralParser<ParseHandler, Unit>::computedPropertyName(
    YieldHandling yieldHandling, const Maybe<DeclarationKind>& maybeDecl,
    PropertyNameContext propertyNameContext, ListNodeType literal) {
  MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::LeftBracket));

  uint32_t begin = pos().begin;

  if (maybeDecl) {
    if (*maybeDecl == DeclarationKind::FormalParameter) {
      pc_->functionBox()->hasParameterExprs = true;
    }
  } else if (propertyNameContext ==
             PropertyNameContext::PropertyNameInLiteral) {
    handler_.setListHasNonConstInitializer(literal);
  }

  Node assignNode;
  MOZ_TRY_VAR(assignNode,
              assignExpr(InAllowed, yieldHandling, TripledotProhibited));

  if (!mustMatchToken(TokenKind::RightBracket, JSMSG_COMP_PROP_UNTERM_EXPR)) {
    return errorResult();
  }
  return handler_.newComputedName(assignNode, begin, pos().end);
}

template <class ParseHandler, typename Unit>
typename ParseHandler::ListNodeResult
GeneralParser<ParseHandler, Unit>::objectLiteral(YieldHandling yieldHandling,
                                                 PossibleError* possibleError) {
  MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::LeftCurly));

  uint32_t openedPos = pos().begin;

  ListNodeType literal;
  MOZ_TRY_VAR(literal, handler_.newObjectLiteral(pos().begin));

  bool seenPrototypeMutation = false;
  bool seenCoverInitializedName = false;
  Maybe<DeclarationKind> declKind = Nothing();
  TaggedParserAtomIndex propAtom;
  for (;;) {
    TokenKind tt;
    if (!tokenStream.peekToken(&tt)) {
      return errorResult();
    }
    if (tt == TokenKind::RightCurly) {
      break;
    }

    if (tt == TokenKind::TripleDot) {
      tokenStream.consumeKnownToken(TokenKind::TripleDot);
      uint32_t begin = pos().begin;

      TokenPos innerPos;
      if (!tokenStream.peekTokenPos(&innerPos, TokenStream::SlashIsRegExp)) {
        return errorResult();
      }

      PossibleError possibleErrorInner(*this);
      Node inner;
      MOZ_TRY_VAR(inner, assignExpr(InAllowed, yieldHandling,
                                    TripledotProhibited, &possibleErrorInner));
      if (!checkDestructuringAssignmentTarget(
              inner, innerPos, &possibleErrorInner, possibleError,
              TargetBehavior::ForbidAssignmentPattern)) {
        return errorResult();
      }
      if (!handler_.addSpreadProperty(literal, begin, inner)) {
        return errorResult();
      }
    } else {
      TokenPos namePos = anyChars.nextToken().pos;

      PropertyType propType;
      Node propName;
      MOZ_TRY_VAR(propName, propertyOrMethodName(
                                yieldHandling, PropertyNameInLiteral, declKind,
                                literal, &propType, &propAtom));

      if (propType == PropertyType::Normal) {
        TokenPos exprPos;
        if (!tokenStream.peekTokenPos(&exprPos, TokenStream::SlashIsRegExp)) {
          return errorResult();
        }

        PossibleError possibleErrorInner(*this);
        Node propExpr;
        MOZ_TRY_VAR(propExpr,
                    assignExpr(InAllowed, yieldHandling, TripledotProhibited,
                               &possibleErrorInner));

        if (!checkDestructuringAssignmentElement(
                propExpr, exprPos, &possibleErrorInner, possibleError)) {
          return errorResult();
        }

        if (propAtom == TaggedParserAtomIndex::WellKnown::proto_()) {
          if (seenPrototypeMutation) {
            // Directly report the error when we're definitely not
            // in a destructuring context.
            if (!possibleError) {
              errorAt(namePos.begin, JSMSG_DUPLICATE_PROTO_PROPERTY);
              return errorResult();
            }

            // Otherwise delay error reporting until we've
            // determined whether or not we're destructuring.
            possibleError->setPendingExpressionErrorAt(
                namePos, JSMSG_DUPLICATE_PROTO_PROPERTY);
          }
          seenPrototypeMutation = true;

          // This occurs *only* if we observe PropertyType::Normal!
          // Only |__proto__: v| mutates [[Prototype]]. Getters,
          // setters, method/generator definitions, computed
          // property name versions of all of these, and shorthands
          // do not.
          if (!handler_.addPrototypeMutation(literal, namePos.begin,
                                             propExpr)) {
            return errorResult();
          }
        } else {
          BinaryNodeType propDef;
          MOZ_TRY_VAR(propDef,
                      handler_.newPropertyDefinition(propName, propExpr));

          handler_.addPropertyDefinition(literal, propDef);
        }
      } else if (propType == PropertyType::Shorthand) {
        /*
         * Support, e.g., |({x, y} = o)| as destructuring shorthand
         * for |({x: x, y: y} = o)|, and |var o = {x, y}| as
         * initializer shorthand for |var o = {x: x, y: y}|.
         */
        TaggedParserAtomIndex name = identifierReference(yieldHandling);
        if (!name) {
          return errorResult();
        }

        NameNodeType nameExpr;
        MOZ_TRY_VAR(nameExpr, identifierReference(name));

        if (possibleError) {
          checkDestructuringAssignmentName(nameExpr, namePos, possibleError);
        }

        if (!handler_.addShorthand(literal, handler_.asNameNode(propName),
                                   nameExpr)) {
          return errorResult();
        }
      } else if (propType == PropertyType::CoverInitializedName) {
        /*
         * Support, e.g., |({x=1, y=2} = o)| as destructuring
         * shorthand with default values, as per ES6 12.14.5
         */
        TaggedParserAtomIndex name = identifierReference(yieldHandling);
        if (!name) {
          return errorResult();
        }

        Node lhs;
        MOZ_TRY_VAR(lhs, identifierReference(name));

        tokenStream.consumeKnownToken(TokenKind::Assign);

        if (!seenCoverInitializedName) {
          // "shorthand default" or "CoverInitializedName" syntax is
          // only valid in the case of destructuring.
          seenCoverInitializedName = true;

          if (!possibleError) {
            // Destructuring defaults are definitely not allowed
            // in this object literal, because of something the
            // caller knows about the preceding code. For example,
            // maybe the preceding token is an operator:
            // |x + {y=z}|.
            error(JSMSG_COLON_AFTER_ID);
            return errorResult();
          }

          // Here we set a pending error so that later in the parse,
          // once we've determined whether or not we're
          // destructuring, the error can be reported or ignored
          // appropriately.
          possibleError->setPendingExpressionErrorAt(pos(),
                                                     JSMSG_COLON_AFTER_ID);
        }

        if (const char* chars = nameIsArgumentsOrEval(lhs)) {
          // |chars| is "arguments" or "eval" here.
          if (!strictModeErrorAt(namePos.begin, JSMSG_BAD_STRICT_ASSIGN,
                                 chars)) {
            return errorResult();
          }
        }

        if (handler_.isArgumentsLength(lhs)) {
          pc_->sc()->setIneligibleForArgumentsLength();
        }

        Node rhs;
        MOZ_TRY_VAR(rhs,
                    assignExpr(InAllowed, yieldHandling, TripledotProhibited));

        BinaryNodeType propExpr;
        MOZ_TRY_VAR(propExpr, handler_.newAssignment(ParseNodeKind::AssignExpr,
                                                     lhs, rhs));

        if (!handler_.addPropertyDefinition(literal, propName, propExpr)) {
          return errorResult();
        }
      } else {
        TaggedParserAtomIndex funName;
        bool hasStaticName =
            !anyChars.isCurrentTokenType(TokenKind::RightBracket) && propAtom;
        if (hasStaticName) {
          funName = propAtom;

          if (propType == PropertyType::Getter ||
              propType == PropertyType::Setter) {
            funName = prefixAccessorName(propType, propAtom);
            if (!funName) {
              return errorResult();
            }
          }
        }

        FunctionNodeType funNode;
        MOZ_TRY_VAR(funNode,
                    methodDefinition(namePos.begin, propType, funName));

        AccessorType atype = ToAccessorType(propType);
        if (!handler_.addObjectMethodDefinition(literal, propName, funNode,
                                                atype)) {
          return errorResult();
        }

        if (possibleError) {
          possibleError->setPendingDestructuringErrorAt(
              namePos, JSMSG_BAD_DESTRUCT_TARGET);
        }
      }
    }

    bool matched;
    if (!tokenStream.matchToken(&matched, TokenKind::Comma,
                                TokenStream::SlashIsInvalid)) {
      return errorResult();
    }
    if (!matched) {
      break;
    }
    if (tt == TokenKind::TripleDot && possibleError) {
      possibleError->setPendingDestructuringErrorAt(pos(),
                                                    JSMSG_REST_WITH_COMMA);
    }
  }

  if (!mustMatchToken(
          TokenKind::RightCurly, [this, openedPos](TokenKind actual) {
            this->reportMissingClosing(JSMSG_CURLY_AFTER_LIST,
                                       JSMSG_CURLY_OPENED, openedPos);
          })) {
    return errorResult();
  }

  handler_.setEndPosition(literal, pos().end);
  return literal;
}

#ifdef ENABLE_RECORD_TUPLE
template <class ParseHandler, typename Unit>
typename ParseHandler::ListNodeResult
GeneralParser<ParseHandler, Unit>::recordLiteral(YieldHandling yieldHandling) {
  MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::HashCurly));

  uint32_t openedPos = pos().begin;

  ListNodeType literal;
  MOZ_TRY_VAR(literal, handler_.newRecordLiteral(pos().begin));

  TaggedParserAtomIndex propAtom;
  for (;;) {
    TokenKind tt;
    if (!tokenStream.peekToken(&tt)) {
      return errorResult();
    }
    if (tt == TokenKind::RightCurly) {
      break;
    }

    if (tt == TokenKind::TripleDot) {
      tokenStream.consumeKnownToken(TokenKind::TripleDot);
      uint32_t begin = pos().begin;

      TokenPos innerPos;
      if (!tokenStream.peekTokenPos(&innerPos, TokenStream::SlashIsRegExp)) {
        return errorResult();
      }

      Node inner;
      MOZ_TRY_VAR(inner,
                  assignExpr(InAllowed, yieldHandling, TripledotProhibited));

      if (!handler_.addSpreadProperty(literal, begin, inner)) {
        return errorResult();
      }
    } else {
      TokenPos namePos = anyChars.nextToken().pos;

      PropertyType propType;
      Node propName;
      MOZ_TRY_VAR(propName,
                  propertyOrMethodName(yieldHandling, PropertyNameInRecord,
                                       /* maybeDecl */ Nothing(), literal,
                                       &propType, &propAtom));

      if (propType == PropertyType::Normal) {
        TokenPos exprPos;
        if (!tokenStream.peekTokenPos(&exprPos, TokenStream::SlashIsRegExp)) {
          return errorResult();
        }

        Node propExpr;
        MOZ_TRY_VAR(propExpr,
                    assignExpr(InAllowed, yieldHandling, TripledotProhibited));

        if (propAtom == TaggedParserAtomIndex::WellKnown::proto_()) {
          errorAt(namePos.begin, JSMSG_RECORD_NO_PROTO);
          return errorResult();
        }

        BinaryNodeType propDef;
        MOZ_TRY_VAR(propDef,
                    handler_.newPropertyDefinition(propName, propExpr));

        handler_.addPropertyDefinition(literal, propDef);
      } else if (propType == PropertyType::Shorthand) {
        /*
         * Support |var o = #{x, y}| as initializer shorthand for
         * |var o = #{x: x, y: y}|.
         */
        TaggedParserAtomIndex name = identifierReference(yieldHandling);
        if (!name) {
          return errorResult();
        }

        NameNodeType nameExpr;
        MOZ_TRY_VAR(nameExpr, identifierReference(name));

        if (!handler_.addShorthand(literal, handler_.asNameNode(propName),
                                   nameExpr)) {
          return errorResult();
        }
      } else {
        error(JSMSG_BAD_PROP_ID);
        return errorResult();
      }
    }

    bool matched;
    if (!tokenStream.matchToken(&matched, TokenKind::Comma,
                                TokenStream::SlashIsInvalid)) {
      return errorResult();
    }
    if (!matched) {
      break;
    }
  }

  if (!mustMatchToken(
          TokenKind::RightCurly, [this, openedPos](TokenKind actual) {
            this->reportMissingClosing(JSMSG_CURLY_AFTER_LIST,
                                       JSMSG_CURLY_OPENED, openedPos);
          })) {
    return errorResult();
  }

  handler_.setEndPosition(literal, pos().end);
  return literal;
}

template <class ParseHandler, typename Unit>
typename ParseHandler::ListNodeResult
GeneralParser<ParseHandler, Unit>::tupleLiteral(YieldHandling yieldHandling) {
  MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::HashBracket));

  uint32_t begin = pos().begin;
  ListNodeType literal;
  MOZ_TRY_VAR(literal, handler_.newTupleLiteral(begin));

  for (uint32_t index = 0;; index++) {
    if (index >= NativeObject::MAX_DENSE_ELEMENTS_COUNT) {
      error(JSMSG_ARRAY_INIT_TOO_BIG);
      return errorResult();
    }

    TokenKind tt;
    if (!tokenStream.peekToken(&tt, TokenStream::SlashIsRegExp)) {
      return errorResult();
    }
    if (tt == TokenKind::RightBracket) {
      break;
    }

    if (tt == TokenKind::TripleDot) {
      tokenStream.consumeKnownToken(TokenKind::TripleDot,
                                    TokenStream::SlashIsRegExp);
      uint32_t begin = pos().begin;

      TokenPos innerPos;
      if (!tokenStream.peekTokenPos(&innerPos, TokenStream::SlashIsRegExp)) {
        return errorResult();
      }

      Node inner;
      MOZ_TRY_VAR(inner,
                  assignExpr(InAllowed, yieldHandling, TripledotProhibited));

      if (!handler_.addSpreadElement(literal, begin, inner)) {
        return errorResult();
      }
    } else {
      TokenPos elementPos;
      if (!tokenStream.peekTokenPos(&elementPos, TokenStream::SlashIsRegExp)) {
        return errorResult();
      }

      Node element;
      MOZ_TRY_VAR(element,
                  assignExpr(InAllowed, yieldHandling, TripledotProhibited));
      handler_.addArrayElement(literal, element);
    }

    bool matched;
    if (!tokenStream.matchToken(&matched, TokenKind::Comma,
                                TokenStream::SlashIsRegExp)) {
      return errorResult();
    }
    if (!matched) {
      break;
    }
  }

  if (!mustMatchToken(TokenKind::RightBracket, [this, begin](TokenKind actual) {
        this->reportMissingClosing(JSMSG_BRACKET_AFTER_LIST,
                                   JSMSG_BRACKET_OPENED, begin);
      })) {
    return errorResult();
  }

  handler_.setEndPosition(literal, pos().end);
  return literal;
}
#endif

template <class ParseHandler, typename Unit>
typename ParseHandler::FunctionNodeResult
GeneralParser<ParseHandler, Unit>::methodDefinition(
    uint32_t toStringStart, PropertyType propType,
    TaggedParserAtomIndex funName) {
  FunctionSyntaxKind syntaxKind;
  switch (propType) {
    case PropertyType::Getter:
      syntaxKind = FunctionSyntaxKind::Getter;
      break;

    case PropertyType::Setter:
      syntaxKind = FunctionSyntaxKind::Setter;
      break;

    case PropertyType::Method:
    case PropertyType::GeneratorMethod:
    case PropertyType::AsyncMethod:
    case PropertyType::AsyncGeneratorMethod:
      syntaxKind = FunctionSyntaxKind::Method;
      break;

    case PropertyType::Constructor:
      syntaxKind = FunctionSyntaxKind::ClassConstructor;
      break;

    case PropertyType::DerivedConstructor:
      syntaxKind = FunctionSyntaxKind::DerivedClassConstructor;
      break;

    default:
      MOZ_CRASH("unexpected property type");
  }

  GeneratorKind generatorKind = (propType == PropertyType::GeneratorMethod ||
                                 propType == PropertyType::AsyncGeneratorMethod)
                                    ? GeneratorKind::Generator
                                    : GeneratorKind::NotGenerator;

  FunctionAsyncKind asyncKind = (propType == PropertyType::AsyncMethod ||
                                 propType == PropertyType::AsyncGeneratorMethod)
                                    ? FunctionAsyncKind::AsyncFunction
                                    : FunctionAsyncKind::SyncFunction;

  YieldHandling yieldHandling = GetYieldHandling(generatorKind);

  FunctionNodeType funNode;
  MOZ_TRY_VAR(funNode, handler_.newFunction(syntaxKind, pos()));

  return functionDefinition(funNode, toStringStart, InAllowed, yieldHandling,
                            funName, syntaxKind, generatorKind, asyncKind);
}

template <class ParseHandler, typename Unit>
bool GeneralParser<ParseHandler, Unit>::tryNewTarget(
    NewTargetNodeType* newTarget) {
  MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::New));

  *newTarget = null();

  NullaryNodeType newHolder;
  MOZ_TRY_VAR_OR_RETURN(newHolder, handler_.newPosHolder(pos()), false);

  uint32_t begin = pos().begin;

  // |new| expects to look for an operand, so we will honor that.
  TokenKind next;
  if (!tokenStream.getToken(&next, TokenStream::SlashIsRegExp)) {
    return false;
  }

  // Don't unget the token, since lookahead cannot handle someone calling
  // getToken() with a different modifier. Callers should inspect
  // currentToken().
  if (next != TokenKind::Dot) {
    return true;
  }

  if (!tokenStream.getToken(&next)) {
    return false;
  }
  if (next != TokenKind::Target) {
    error(JSMSG_UNEXPECTED_TOKEN, "target", TokenKindToDesc(next));
    return false;
  }

  if (!pc_->sc()->allowNewTarget()) {
    errorAt(begin, JSMSG_BAD_NEWTARGET);
    return false;
  }

  NullaryNodeType targetHolder;
  MOZ_TRY_VAR_OR_RETURN(targetHolder, handler_.newPosHolder(pos()), false);

  NameNodeType newTargetName;
  MOZ_TRY_VAR_OR_RETURN(newTargetName, newNewTargetName(), false);

  MOZ_TRY_VAR_OR_RETURN(
      *newTarget, handler_.newNewTarget(newHolder, targetHolder, newTargetName),
      false);

  return true;
}

template <class ParseHandler, typename Unit>
typename ParseHandler::BinaryNodeResult
GeneralParser<ParseHandler, Unit>::importExpr(YieldHandling yieldHandling,
                                              bool allowCallSyntax) {
  MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::Import));

  NullaryNodeType importHolder;
  MOZ_TRY_VAR(importHolder, handler_.newPosHolder(pos()));

  TokenKind next;
  if (!tokenStream.getToken(&next)) {
    return errorResult();
  }

  if (next == TokenKind::Dot) {
    if (!tokenStream.getToken(&next)) {
      return errorResult();
    }
    if (next != TokenKind::Meta) {
      error(JSMSG_UNEXPECTED_TOKEN, "meta", TokenKindToDesc(next));
      return errorResult();
    }

    if (parseGoal() != ParseGoal::Module) {
      errorAt(pos().begin, JSMSG_IMPORT_META_OUTSIDE_MODULE);
      return errorResult();
    }

    NullaryNodeType metaHolder;
    MOZ_TRY_VAR(metaHolder, handler_.newPosHolder(pos()));

    return handler_.newImportMeta(importHolder, metaHolder);
  }

  if (next == TokenKind::LeftParen && allowCallSyntax) {
    Node arg;
    MOZ_TRY_VAR(arg, assignExpr(InAllowed, yieldHandling, TripledotProhibited));

    if (!tokenStream.peekToken(&next, TokenStream::SlashIsRegExp)) {
      return errorResult();
    }

    Node optionalArg;
    if (options().importAttributes()) {
      if (next == TokenKind::Comma) {
        tokenStream.consumeKnownToken(TokenKind::Comma,
                                      TokenStream::SlashIsRegExp);

        if (!tokenStream.peekToken(&next, TokenStream::SlashIsRegExp)) {
          return errorResult();
        }

        if (next != TokenKind::RightParen) {
          MOZ_TRY_VAR(optionalArg, assignExpr(InAllowed, yieldHandling,
                                              TripledotProhibited));

          if (!tokenStream.peekToken(&next, TokenStream::SlashIsRegExp)) {
            return errorResult();
          }

          if (next == TokenKind::Comma) {
            tokenStream.consumeKnownToken(TokenKind::Comma,
                                          TokenStream::SlashIsRegExp);
          }
        } else {
          MOZ_TRY_VAR(optionalArg,
                      handler_.newPosHolder(TokenPos(pos().end, pos().end)));
        }
      } else {
        MOZ_TRY_VAR(optionalArg,
                    handler_.newPosHolder(TokenPos(pos().end, pos().end)));
      }
    } else {
      MOZ_TRY_VAR(optionalArg,
                  handler_.newPosHolder(TokenPos(pos().end, pos().end)));
    }

    if (!mustMatchToken(TokenKind::RightParen, JSMSG_PAREN_AFTER_ARGS)) {
      return errorResult();
    }

    Node spec;
    MOZ_TRY_VAR(spec, handler_.newCallImportSpec(arg, optionalArg));

    return handler_.newCallImport(importHolder, spec);
  }

  error(JSMSG_UNEXPECTED_TOKEN_NO_EXPECT, TokenKindToDesc(next));
  return errorResult();
}

template <class ParseHandler, typename Unit>
typename ParseHandler::NodeResult
GeneralParser<ParseHandler, Unit>::primaryExpr(
    YieldHandling yieldHandling, TripledotHandling tripledotHandling,
    TokenKind tt, PossibleError* possibleError, InvokedPrediction invoked) {
  MOZ_ASSERT(anyChars.isCurrentTokenType(tt));
  AutoCheckRecursionLimit recursion(this->fc_);
  if (!recursion.check(this->fc_)) {
    return errorResult();
  }

  switch (tt) {
    case TokenKind::Function:
      return functionExpr(pos().begin, invoked,
                          FunctionAsyncKind::SyncFunction);

    case TokenKind::Class:
      return classDefinition(yieldHandling, ClassExpression, NameRequired);

    case TokenKind::LeftBracket:
      return arrayInitializer(yieldHandling, possibleError);

    case TokenKind::LeftCurly:
      return objectLiteral(yieldHandling, possibleError);

#ifdef ENABLE_RECORD_TUPLE
    case TokenKind::HashCurly:
      return recordLiteral(yieldHandling);

    case TokenKind::HashBracket:
      return tupleLiteral(yieldHandling);
#endif

#ifdef ENABLE_DECORATORS
    case TokenKind::At:
      return classDefinition(yieldHandling, ClassExpression, NameRequired);
#endif

    case TokenKind::LeftParen: {
      TokenKind next;
      if (!tokenStream.peekToken(&next, TokenStream::SlashIsRegExp)) {
        return errorResult();
      }

      if (next == TokenKind::RightParen) {
        // Not valid expression syntax, but this is valid in an arrow function
        // with no params: `() => body`.
        tokenStream.consumeKnownToken(TokenKind::RightParen,
                                      TokenStream::SlashIsRegExp);

        if (!tokenStream.peekToken(&next)) {
          return errorResult();
        }
        if (next != TokenKind::Arrow) {
          error(JSMSG_UNEXPECTED_TOKEN, "expression",
                TokenKindToDesc(TokenKind::RightParen));
          return errorResult();
        }

        // Now just return something that will allow parsing to continue.
        // It doesn't matter what; when we reach the =>, we will rewind and
        // reparse the whole arrow function. See Parser::assignExpr.
        return handler_.newNullLiteral(pos());
      }

      // Pass |possibleError| to support destructuring in arrow parameters.
      Node expr;
      MOZ_TRY_VAR(expr, exprInParens(InAllowed, yieldHandling, TripledotAllowed,
                                     possibleError));
      if (!mustMatchToken(TokenKind::RightParen, JSMSG_PAREN_IN_PAREN)) {
        return errorResult();
      }
      return handler_.parenthesize(expr);
    }

    case TokenKind::TemplateHead:
      return templateLiteral(yieldHandling);

    case TokenKind::NoSubsTemplate:
      return noSubstitutionUntaggedTemplate();

    case TokenKind::String:
      return stringLiteral();

    default: {
      if (!TokenKindIsPossibleIdentifier(tt)) {
        error(JSMSG_UNEXPECTED_TOKEN, "expression", TokenKindToDesc(tt));
        return errorResult();
      }

      if (tt == TokenKind::Async) {
        TokenKind nextSameLine = TokenKind::Eof;
        if (!tokenStream.peekTokenSameLine(&nextSameLine)) {
          return errorResult();
        }

        if (nextSameLine == TokenKind::Function) {
          uint32_t toStringStart = pos().begin;
          tokenStream.consumeKnownToken(TokenKind::Function);
          return functionExpr(toStringStart, PredictUninvoked,
                              FunctionAsyncKind::AsyncFunction);
        }
      }

      TaggedParserAtomIndex name = identifierReference(yieldHandling);
      if (!name) {
        return errorResult();
      }

      return identifierReference(name);
    }

    case TokenKind::RegExp:
      return newRegExp();

    case TokenKind::Number:
      return newNumber(anyChars.currentToken());

    case TokenKind::BigInt:
      return newBigInt();

    case TokenKind::True:
      return handler_.newBooleanLiteral(true, pos());
    case TokenKind::False:
      return handler_.newBooleanLiteral(false, pos());
    case TokenKind::This: {
      NameNodeType thisName = null();
      if (pc_->sc()->hasFunctionThisBinding()) {
        MOZ_TRY_VAR(thisName, newThisName());
      }
      return handler_.newThisLiteral(pos(), thisName);
    }
    case TokenKind::Null:
      return handler_.newNullLiteral(pos());

    case TokenKind::TripleDot: {
      // This isn't valid expression syntax, but it's valid in an arrow
      // function as a trailing rest param: `(a, b, ...rest) => body`.  Check
      // if it's directly under
      // CoverParenthesizedExpressionAndArrowParameterList, and check for a
      // name, closing parenthesis, and arrow, and allow it only if all are
      // present.
      if (tripledotHandling != TripledotAllowed) {
        error(JSMSG_UNEXPECTED_TOKEN, "expression", TokenKindToDesc(tt));
        return errorResult();
      }

      TokenKind next;
      if (!tokenStream.getToken(&next)) {
        return errorResult();
      }

      if (next == TokenKind::LeftBracket || next == TokenKind::LeftCurly) {
        // Validate, but don't store the pattern right now. The whole arrow
        // function is reparsed in functionFormalParametersAndBody().
        MOZ_TRY(destructuringDeclaration(DeclarationKind::CoverArrowParameter,
                                         yieldHandling, next));
      } else {
        // This doesn't check that the provided name is allowed, e.g. if
        // the enclosing code is strict mode code, any of "let", "yield",
        // or "arguments" should be prohibited.  Argument-parsing code
        // handles that.
        if (!TokenKindIsPossibleIdentifier(next)) {
          error(JSMSG_UNEXPECTED_TOKEN, "rest argument name",
                TokenKindToDesc(next));
          return errorResult();
        }
      }

      if (!tokenStream.getToken(&next)) {
        return errorResult();
      }
      if (next != TokenKind::RightParen) {
        error(JSMSG_UNEXPECTED_TOKEN, "closing parenthesis",
              TokenKindToDesc(next));
        return errorResult();
      }

      if (!tokenStream.peekToken(&next)) {
        return errorResult();
      }
      if (next != TokenKind::Arrow) {
        // Advance the scanner for proper error location reporting.
        tokenStream.consumeKnownToken(next);
        error(JSMSG_UNEXPECTED_TOKEN, "'=>' after argument list",
              TokenKindToDesc(next));
        return errorResult();
      }

      anyChars.ungetToken();  // put back right paren

      // Return an arbitrary expression node. See case TokenKind::RightParen
      // above.
      return handler_.newNullLiteral(pos());
    }
  }
}

template <class ParseHandler, typename Unit>
typename ParseHandler::NodeResult
GeneralParser<ParseHandler, Unit>::exprInParens(
    InHandling inHandling, YieldHandling yieldHandling,
    TripledotHandling tripledotHandling,
    PossibleError* possibleError /* = nullptr */) {
  MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::LeftParen));
  return expr(inHandling, yieldHandling, tripledotHandling, possibleError,
              PredictInvoked);
}

template class PerHandlerParser<FullParseHandler>;
template class PerHandlerParser<SyntaxParseHandler>;
template class GeneralParser<FullParseHandler, Utf8Unit>;
template class GeneralParser<SyntaxParseHandler, Utf8Unit>;
template class GeneralParser<FullParseHandler, char16_t>;
template class GeneralParser<SyntaxParseHandler, char16_t>;
template class Parser<FullParseHandler, Utf8Unit>;
template class Parser<SyntaxParseHandler, Utf8Unit>;
template class Parser<FullParseHandler, char16_t>;
template class Parser<SyntaxParseHandler, char16_t>;

}  // namespace js::frontend
