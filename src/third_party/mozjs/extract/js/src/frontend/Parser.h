/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* JS parser. */

#ifndef frontend_Parser_h
#define frontend_Parser_h

/*
 * [SMDOC] JS Parser
 *
 * JS parsers capable of generating ASTs from source text.
 *
 * A parser embeds token stream information, then gets and matches tokens to
 * generate a syntax tree that, if desired, BytecodeEmitter will use to compile
 * bytecode.
 *
 * Like token streams (see the comment near the top of TokenStream.h), parser
 * classes are heavily templatized -- along the token stream's character-type
 * axis, and also along a full-parse/syntax-parse axis.  Certain limitations of
 * C++ (primarily the inability to partially specialize function templates),
 * plus the desire to minimize compiled code size in duplicate function
 * template instantiations wherever possible, mean that Parser exhibits much of
 * the same unholy template/inheritance complexity as token streams.
 *
 * == ParserSharedBase ==
 *
 * ParserSharedBase is the base class for both regular JS and BinAST parsing.
 * This class contains common fields and methods between both parsers. There is
 * currently no BinAST parser here so this can potentially be merged into the
 * ParserBase type below.
 *
 * == ParserBase → ParserSharedBase, ErrorReportMixin ==
 *
 * ParserBase is the base class for regular JS parser, shared by all regular JS
 * parsers of all character types and parse-handling behavior.  It stores
 * everything character- and handler-agnostic.
 *
 * ParserBase's most important field is the parser's token stream's
 * |TokenStreamAnyChars| component, for all tokenizing aspects that are
 * character-type-agnostic.  The character-type-sensitive components residing
 * in |TokenStreamSpecific| (see the comment near the top of TokenStream.h)
 * live elsewhere in this hierarchy.  These separate locations are the reason
 * for the |AnyCharsAccess| template parameter to |TokenStreamChars| and
 * |TokenStreamSpecific|.
 *
 * == PerHandlerParser<ParseHandler> → ParserBase ==
 *
 * Certain parsing behavior varies between full parsing and syntax-only parsing
 * but does not vary across source-text character types.  For example, the work
 * to "create an arguments object for a function" obviously varies between
 * syntax and full parsing but (because no source characters are examined) does
 * not vary by source text character type.  Such functionality is implemented
 * through functions in PerHandlerParser.
 *
 * Functionality only used by syntax parsing or full parsing doesn't live here:
 * it should be implemented in the appropriate Parser<ParseHandler> (described
 * further below).
 *
 * == GeneralParser<ParseHandler, Unit> → PerHandlerParser<ParseHandler> ==
 *
 * Most parsing behavior varies across the character-type axis (and possibly
 * along the full/syntax axis).  For example:
 *
 *   * Parsing ECMAScript's Expression production, implemented by
 *     GeneralParser::expr, varies in this manner: different types are used to
 *     represent nodes in full and syntax parsing (ParseNode* versus an enum),
 *     and reading the tokens comprising the expression requires inspecting
 *     individual characters (necessarily dependent upon character type).
 *   * Reporting an error or warning does not depend on the full/syntax parsing
 *     distinction.  But error reports and warnings include a line of context
 *     (or a slice of one), for pointing out where a mistake was made.
 *     Computing such line of context requires inspecting the source text to
 *     make that line/slice of context, which requires knowing the source text
 *     character type.
 *
 * Such functionality, implemented using identical function code across these
 * axes, should live in GeneralParser.
 *
 * GeneralParser's most important field is the parser's token stream's
 * |TokenStreamSpecific| component, for all aspects of tokenizing that (contra
 * |TokenStreamAnyChars| in ParserBase above) are character-type-sensitive.  As
 * noted above, this field's existence separate from that in ParserBase
 * motivates the |AnyCharsAccess| template parameters on various token stream
 * classes.
 *
 * Everything in PerHandlerParser *could* be folded into GeneralParser (below)
 * if desired.  We don't fold in this manner because all such functions would
 * be instantiated once per Unit -- but if exactly equivalent code would be
 * generated (because PerHandlerParser functions have no awareness of Unit),
 * it's risky to *depend* upon the compiler coalescing the instantiations into
 * one in the final binary.  PerHandlerParser guarantees no duplication.
 *
 * == Parser<ParseHandler, Unit> final → GeneralParser<ParseHandler, Unit> ==
 *
 * The final (pun intended) axis of complexity lies in Parser.
 *
 * Some functionality depends on character type, yet also is defined in
 * significantly different form in full and syntax parsing.  For example,
 * attempting to parse the source text of a module will do so in full parsing
 * but immediately fail in syntax parsing -- so the former is a mess'o'code
 * while the latter is effectively |return null();|.  Such functionality is
 * defined in Parser<SyntaxParseHandler or FullParseHandler, Unit> as
 * appropriate.
 *
 * There's a crucial distinction between GeneralParser and Parser, that
 * explains why both must exist (despite taking exactly the same template
 * parameters, and despite GeneralParser and Parser existing in a one-to-one
 * relationship).  GeneralParser is one unspecialized template class:
 *
 *   template<class ParseHandler, typename Unit>
 *   class GeneralParser : ...
 *   {
 *     ...parsing functions...
 *   };
 *
 * but Parser is one undefined template class with two separate
 * specializations:
 *
 *   // Declare, but do not define.
 *   template<class ParseHandler, typename Unit> class Parser;
 *
 *   // Define a syntax-parsing specialization.
 *   template<typename Unit>
 *   class Parser<SyntaxParseHandler, Unit> final
 *     : public GeneralParser<SyntaxParseHandler, Unit>
 *   {
 *     ...parsing functions...
 *   };
 *
 *   // Define a full-parsing specialization.
 *   template<typename Unit>
 *   class Parser<SyntaxParseHandler, Unit> final
 *     : public GeneralParser<SyntaxParseHandler, Unit>
 *   {
 *     ...parsing functions...
 *   };
 *
 * This odd distinction is necessary because C++ unfortunately doesn't allow
 * partial function specialization:
 *
 *   // BAD: You can only specialize a template function if you specify *every*
 *   //      template parameter, i.e. ParseHandler *and* Unit.
 *   template<typename Unit>
 *   void
 *   GeneralParser<SyntaxParseHandler, Unit>::foo() {}
 *
 * But if you specialize Parser *as a class*, then this is allowed:
 *
 *   template<typename Unit>
 *   void
 *   Parser<SyntaxParseHandler, Unit>::foo() {}
 *
 *   template<typename Unit>
 *   void
 *   Parser<FullParseHandler, Unit>::foo() {}
 *
 * because the only template parameter on the function is Unit -- and so all
 * template parameters *are* varying, not a strict subset of them.
 *
 * So -- any parsing functionality that is differently defined for different
 * ParseHandlers, *but* is defined textually identically for different Unit
 * (even if different code ends up generated for them by the compiler), should
 * reside in Parser.
 */

#include "mozilla/Maybe.h"

#include <type_traits>
#include <utility>

#include "frontend/CompilationStencil.h"  // CompilationState
#include "frontend/ErrorReporter.h"
#include "frontend/FullParseHandler.h"
#include "frontend/FunctionSyntaxKind.h"  // FunctionSyntaxKind
#include "frontend/IteratorKind.h"
#include "frontend/NameAnalysisTypes.h"
#include "frontend/ParseContext.h"
#include "frontend/ParserAtom.h"  // ParserAtomsTable, TaggedParserAtomIndex
#include "frontend/SharedContext.h"
#include "frontend/SyntaxParseHandler.h"
#include "frontend/TokenStream.h"
#include "js/CharacterEncoding.h"     // JS::ConstUTF8CharsZ
#include "js/friend/ErrorMessages.h"  // JSErrNum, JSMSG_*
#include "vm/GeneratorAndAsyncKind.h"  // js::GeneratorKind, js::FunctionAsyncKind

namespace js {

class FrontendContext;
struct ErrorMetadata;

namespace frontend {

template <class ParseHandler, typename Unit>
class GeneralParser;

class SourceParseContext : public ParseContext {
 public:
  template <typename ParseHandler, typename Unit>
  SourceParseContext(GeneralParser<ParseHandler, Unit>* prs, SharedContext* sc,
                     Directives* newDirectives)
      : ParseContext(prs->fc_, prs->pc_, sc, prs->tokenStream,
                     prs->compilationState_, newDirectives,
                     std::is_same_v<ParseHandler, FullParseHandler>) {}
};

enum VarContext { HoistVars, DontHoistVars };
enum PropListType { ObjectLiteral, ClassBody, DerivedClassBody };
enum class PropertyType {
  Normal,
  Shorthand,
  CoverInitializedName,
  Getter,
  Setter,
  Method,
  GeneratorMethod,
  AsyncMethod,
  AsyncGeneratorMethod,
  Constructor,
  DerivedConstructor,
  Field,
  FieldWithAccessor,
};

enum AwaitHandling : uint8_t {
  AwaitIsName,
  AwaitIsKeyword,
  AwaitIsModuleKeyword,
  AwaitIsDisallowed
};

template <class ParseHandler, typename Unit>
class AutoAwaitIsKeyword;

template <class ParseHandler, typename Unit>
class AutoInParametersOfAsyncFunction;

class MOZ_STACK_CLASS ParserSharedBase {
 public:
  enum class Kind { Parser };

  ParserSharedBase(FrontendContext* fc, CompilationState& compilationState,
                   Kind kind);
  ~ParserSharedBase();

 public:
  FrontendContext* fc_;

  LifoAlloc& alloc_;

  CompilationState& compilationState_;

  // innermost parse context (stack-allocated)
  ParseContext* pc_;

  // For tracking used names in this parsing session.
  UsedNameTracker& usedNames_;

 public:
  CompilationState& getCompilationState() { return compilationState_; }

  ParserAtomsTable& parserAtoms() { return compilationState_.parserAtoms; }
  const ParserAtomsTable& parserAtoms() const {
    return compilationState_.parserAtoms;
  }

  LifoAlloc& stencilAlloc() { return compilationState_.alloc; }

  const UsedNameTracker& usedNames() { return usedNames_; }

#if defined(DEBUG) || defined(JS_JITSPEW)
  void dumpAtom(TaggedParserAtomIndex index) const;
#endif
};

class MOZ_STACK_CLASS ParserBase : public ParserSharedBase,
                                   public ErrorReportMixin {
  using Base = ErrorReportMixin;

 public:
  TokenStreamAnyChars anyChars;

  ScriptSource* ss;

  // Perform constant-folding; must be true when interfacing with the emitter.
  const bool foldConstants_ : 1;

 protected:
#if DEBUG
  /* Our fallible 'checkOptions' member function has been called. */
  bool checkOptionsCalled_ : 1;
#endif

  /* Unexpected end of input, i.e. Eof not at top-level. */
  bool isUnexpectedEOF_ : 1;

  /* AwaitHandling */ uint8_t awaitHandling_ : 2;

  bool inParametersOfAsyncFunction_ : 1;

 public:
  JSAtom* liftParserAtomToJSAtom(TaggedParserAtomIndex index);

  bool awaitIsKeyword() const {
    return awaitHandling_ == AwaitIsKeyword ||
           awaitHandling_ == AwaitIsModuleKeyword;
  }
  bool awaitIsDisallowed() const { return awaitHandling_ == AwaitIsDisallowed; }

  bool inParametersOfAsyncFunction() const {
    return inParametersOfAsyncFunction_;
  }

  ParseGoal parseGoal() const {
    return pc_->sc()->hasModuleGoal() ? ParseGoal::Module : ParseGoal::Script;
  }

  template <class, typename>
  friend class AutoAwaitIsKeyword;
  template <class, typename>
  friend class AutoInParametersOfAsyncFunction;

  ParserBase(FrontendContext* fc, const JS::ReadOnlyCompileOptions& options,
             bool foldConstants, CompilationState& compilationState);
  ~ParserBase();

  bool checkOptions();

  JS::ConstUTF8CharsZ getFilename() const { return anyChars.getFilename(); }
  TokenPos pos() const { return anyChars.currentToken().pos; }

  // Determine whether |yield| is a valid name in the current context.
  bool yieldExpressionsSupported() const { return pc_->isGenerator(); }

  bool setLocalStrictMode(bool strict) {
    MOZ_ASSERT(anyChars.debugHasNoLookahead());
    return pc_->sc()->setLocalStrictMode(strict);
  }

 public:
  // Implement ErrorReportMixin.

  FrontendContext* getContext() const override { return fc_; }

  bool strictMode() const override { return pc_->sc()->strict(); }

  const JS::ReadOnlyCompileOptions& options() const override {
    return anyChars.options();
  }

  using Base::error;
  using Base::errorAt;
  using Base::errorNoOffset;
  using Base::errorWithNotes;
  using Base::errorWithNotesAt;
  using Base::errorWithNotesNoOffset;
  using Base::strictModeError;
  using Base::strictModeErrorAt;
  using Base::strictModeErrorNoOffset;
  using Base::strictModeErrorWithNotes;
  using Base::strictModeErrorWithNotesAt;
  using Base::strictModeErrorWithNotesNoOffset;
  using Base::warning;
  using Base::warningAt;
  using Base::warningNoOffset;

 public:
  bool isUnexpectedEOF() const { return isUnexpectedEOF_; }

  bool isValidStrictBinding(TaggedParserAtomIndex name);

  bool hasValidSimpleStrictParameterNames();

  // A Parser::Mark is the extension of the LifoAlloc::Mark to the entire
  // Parser's state. Note: clients must still take care that any ParseContext
  // that points into released ParseNodes is destroyed.
  class Mark {
    friend class ParserBase;
    LifoAlloc::Mark mark;
    CompilationState::CompilationStatePosition pos;
  };
  Mark mark() const {
    Mark m;
    m.mark = alloc_.mark();
    m.pos = compilationState_.getPosition();
    return m;
  }
  void release(Mark m) {
    alloc_.release(m.mark);
    compilationState_.rewind(m.pos);
  }

 public:
  mozilla::Maybe<GlobalScope::ParserData*> newGlobalScopeData(
      ParseContext::Scope& scope);
  mozilla::Maybe<ModuleScope::ParserData*> newModuleScopeData(
      ParseContext::Scope& scope);
  mozilla::Maybe<EvalScope::ParserData*> newEvalScopeData(
      ParseContext::Scope& scope);
  mozilla::Maybe<FunctionScope::ParserData*> newFunctionScopeData(
      ParseContext::Scope& scope, bool hasParameterExprs);
  mozilla::Maybe<VarScope::ParserData*> newVarScopeData(
      ParseContext::Scope& scope);
  mozilla::Maybe<LexicalScope::ParserData*> newLexicalScopeData(
      ParseContext::Scope& scope);
  mozilla::Maybe<ClassBodyScope::ParserData*> newClassBodyScopeData(
      ParseContext::Scope& scope);

 protected:
  enum InvokedPrediction { PredictUninvoked = false, PredictInvoked = true };
  enum ForInitLocation { InForInit, NotInForInit };

  // While on a |let| Name token, examine |next| (which must already be
  // gotten).  Indicate whether |next|, the next token already gotten with
  // modifier TokenStream::SlashIsDiv, continues a LexicalDeclaration.
  bool nextTokenContinuesLetDeclaration(TokenKind next);

  bool noteUsedNameInternal(TaggedParserAtomIndex name,
                            NameVisibility visibility,
                            mozilla::Maybe<TokenPos> tokenPosition);

  bool checkAndMarkSuperScope();

  bool leaveInnerFunction(ParseContext* outerpc);

  TaggedParserAtomIndex prefixAccessorName(PropertyType propType,
                                           TaggedParserAtomIndex propAtom);

  [[nodiscard]] bool setSourceMapInfo();

  void setFunctionEndFromCurrentToken(FunctionBox* funbox) const;
};

template <class ParseHandler>
class MOZ_STACK_CLASS PerHandlerParser : public ParserBase {
  using Base = ParserBase;

 private:
  using Node = typename ParseHandler::Node;
  using NodeResult = typename ParseHandler::NodeResult;

#define DECLARE_TYPE(typeName)                                  \
  using typeName##Type = typename ParseHandler::typeName##Type; \
  using typeName##Result = typename ParseHandler::typeName##Result;
  FOR_EACH_PARSENODE_SUBCLASS(DECLARE_TYPE)
#undef DECLARE_TYPE

 protected:
  /* State specific to the kind of parse being performed. */
  ParseHandler handler_;

  // When ParseHandler is FullParseHandler:
  //
  //   If non-null, this field holds the syntax parser used to attempt lazy
  //   parsing of inner functions. If null, then lazy parsing is disabled.
  //
  // When ParseHandler is SyntaxParseHandler:
  //
  //   If non-null, this field must be a sentinel value signaling that the
  //   syntax parse was aborted. If null, then lazy parsing was aborted due
  //   to encountering unsupported language constructs.
  //
  // |internalSyntaxParser_| is really a |Parser<SyntaxParseHandler, Unit>*|
  // where |Unit| varies per |Parser<ParseHandler, Unit>|.  But this
  // template class doesn't know |Unit|, so we store a |void*| here and make
  // |GeneralParser<ParseHandler, Unit>::getSyntaxParser| impose the real type.
  void* internalSyntaxParser_;

 private:
  // NOTE: The argument ordering here is deliberately different from the
  //       public constructor so that typos calling the public constructor
  //       are less likely to select this overload.
  PerHandlerParser(FrontendContext* fc,
                   const JS::ReadOnlyCompileOptions& options,
                   bool foldConstants, CompilationState& compilationState,
                   void* internalSyntaxParser);

 protected:
  template <typename Unit>
  PerHandlerParser(FrontendContext* fc,
                   const JS::ReadOnlyCompileOptions& options,
                   bool foldConstants, CompilationState& compilationState,
                   GeneralParser<SyntaxParseHandler, Unit>* syntaxParser)
      : PerHandlerParser(fc, options, foldConstants, compilationState,
                         static_cast<void*>(syntaxParser)) {}

  static typename ParseHandler::NullNode null() { return ParseHandler::null(); }

  // The return value for the error case in the functions that returns
  // Result type.
  static constexpr typename ParseHandler::NodeErrorResult errorResult() {
    return ParseHandler::errorResult();
  }

  NameNodeResult stringLiteral();

  const char* nameIsArgumentsOrEval(Node node);

  bool noteDestructuredPositionalFormalParameter(FunctionNodeType funNode,
                                                 Node destruct);

  bool noteUsedName(
      TaggedParserAtomIndex name,
      NameVisibility visibility = NameVisibility::Public,
      mozilla::Maybe<TokenPos> tokenPosition = mozilla::Nothing()) {
    // If the we are delazifying, the BaseScript already has all the closed-over
    // info for bindings and there's no need to track used names.
    if (handler_.reuseClosedOverBindings()) {
      return true;
    }

    return ParserBase::noteUsedNameInternal(name, visibility, tokenPosition);
  }

  // Required on Scope exit.
  bool propagateFreeNamesAndMarkClosedOverBindings(ParseContext::Scope& scope);

  bool checkForUndefinedPrivateFields(EvalSharedContext* evalSc = nullptr);

  bool finishFunctionScopes(bool isStandaloneFunction);
  LexicalScopeNodeResult finishLexicalScope(
      ParseContext::Scope& scope, Node body,
      ScopeKind kind = ScopeKind::Lexical);
  ClassBodyScopeNodeResult finishClassBodyScope(ParseContext::Scope& scope,
                                                ListNodeType body);
  bool finishFunction(bool isStandaloneFunction = false);

  inline NameNodeResult newName(TaggedParserAtomIndex name);
  inline NameNodeResult newName(TaggedParserAtomIndex name, TokenPos pos);

  inline NameNodeResult newPrivateName(TaggedParserAtomIndex name);

  NameNodeResult newInternalDotName(TaggedParserAtomIndex name);
  NameNodeResult newThisName();
  NameNodeResult newNewTargetName();
  NameNodeResult newDotGeneratorName();

  NameNodeResult identifierReference(TaggedParserAtomIndex name);
  NameNodeResult privateNameReference(TaggedParserAtomIndex name);

  NodeResult noSubstitutionTaggedTemplate();

  inline bool processExport(Node node);
  inline bool processExportFrom(BinaryNodeType node);
  inline bool processImport(BinaryNodeType node);

  // If ParseHandler is SyntaxParseHandler:
  //   Do nothing.
  // If ParseHandler is FullParseHandler:
  //   Disable syntax parsing of all future inner functions during this
  //   full-parse.
  inline void disableSyntaxParser();

  // If ParseHandler is SyntaxParseHandler:
  //   Flag the current syntax parse as aborted due to unsupported language
  //   constructs and return false.  Aborting the current syntax parse does
  //   not disable attempts to syntax-parse future inner functions.
  // If ParseHandler is FullParseHandler:
  //    Disable syntax parsing of all future inner functions and return true.
  inline bool abortIfSyntaxParser();

  // If ParseHandler is SyntaxParseHandler:
  //   Return whether the last syntax parse was aborted due to unsupported
  //   language constructs.
  // If ParseHandler is FullParseHandler:
  //   Return false.
  inline bool hadAbortedSyntaxParse();

  // If ParseHandler is SyntaxParseHandler:
  //   Clear whether the last syntax parse was aborted.
  // If ParseHandler is FullParseHandler:
  //   Do nothing.
  inline void clearAbortedSyntaxParse();

 public:
  FunctionBox* newFunctionBox(FunctionNodeType funNode,
                              TaggedParserAtomIndex explicitName,
                              FunctionFlags flags, uint32_t toStringStart,
                              Directives directives,
                              GeneratorKind generatorKind,
                              FunctionAsyncKind asyncKind);

  FunctionBox* newFunctionBox(FunctionNodeType funNode,
                              const ScriptStencil& cachedScriptData,
                              const ScriptStencilExtra& cachedScriptExtra);

 public:
  // ErrorReportMixin.

  using Base::error;
  using Base::errorAt;
  using Base::errorNoOffset;
  using Base::errorWithNotes;
  using Base::errorWithNotesAt;
  using Base::errorWithNotesNoOffset;
  using Base::strictModeError;
  using Base::strictModeErrorAt;
  using Base::strictModeErrorNoOffset;
  using Base::strictModeErrorWithNotes;
  using Base::strictModeErrorWithNotesAt;
  using Base::strictModeErrorWithNotesNoOffset;
  using Base::warning;
  using Base::warningAt;
  using Base::warningNoOffset;
};

#define ABORTED_SYNTAX_PARSE_SENTINEL reinterpret_cast<void*>(0x1)

template <>
inline void PerHandlerParser<SyntaxParseHandler>::disableSyntaxParser() {}

template <>
inline bool PerHandlerParser<SyntaxParseHandler>::abortIfSyntaxParser() {
  internalSyntaxParser_ = ABORTED_SYNTAX_PARSE_SENTINEL;
  return false;
}

template <>
inline bool PerHandlerParser<SyntaxParseHandler>::hadAbortedSyntaxParse() {
  return internalSyntaxParser_ == ABORTED_SYNTAX_PARSE_SENTINEL;
}

template <>
inline void PerHandlerParser<SyntaxParseHandler>::clearAbortedSyntaxParse() {
  internalSyntaxParser_ = nullptr;
}

#undef ABORTED_SYNTAX_PARSE_SENTINEL

// Disable syntax parsing of all future inner functions during this
// full-parse.
template <>
inline void PerHandlerParser<FullParseHandler>::disableSyntaxParser() {
  internalSyntaxParser_ = nullptr;
}

template <>
inline bool PerHandlerParser<FullParseHandler>::abortIfSyntaxParser() {
  disableSyntaxParser();
  return true;
}

template <>
inline bool PerHandlerParser<FullParseHandler>::hadAbortedSyntaxParse() {
  return false;
}

template <>
inline void PerHandlerParser<FullParseHandler>::clearAbortedSyntaxParse() {}

template <class Parser>
class ParserAnyCharsAccess {
 public:
  using TokenStreamSpecific = typename Parser::TokenStream;
  using GeneralTokenStreamChars =
      typename TokenStreamSpecific::GeneralCharsBase;

  static inline TokenStreamAnyChars& anyChars(GeneralTokenStreamChars* ts);
  static inline const TokenStreamAnyChars& anyChars(
      const GeneralTokenStreamChars* ts);
};

// Specify a value for an ES6 grammar parametrization.  We have no enum for
// [Return] because its behavior is almost exactly equivalent to checking
// whether we're in a function box -- easier and simpler than passing an extra
// parameter everywhere.
enum YieldHandling { YieldIsName, YieldIsKeyword };
enum InHandling { InAllowed, InProhibited };
enum DefaultHandling { NameRequired, AllowDefaultName };
enum TripledotHandling { TripledotAllowed, TripledotProhibited };

// For Ergonomic brand checks.
enum PrivateNameHandling { PrivateNameProhibited, PrivateNameAllowed };

template <class ParseHandler, typename Unit>
class Parser;

template <class ParseHandler, typename Unit>
class MOZ_STACK_CLASS GeneralParser : public PerHandlerParser<ParseHandler> {
 public:
  using TokenStream =
      TokenStreamSpecific<Unit, ParserAnyCharsAccess<GeneralParser>>;

 private:
  using Base = PerHandlerParser<ParseHandler>;
  using FinalParser = Parser<ParseHandler, Unit>;
  using Node = typename ParseHandler::Node;
  using NodeResult = typename ParseHandler::NodeResult;

#define DECLARE_TYPE(typeName)                                  \
  using typeName##Type = typename ParseHandler::typeName##Type; \
  using typeName##Result = typename ParseHandler::typeName##Result;
  FOR_EACH_PARSENODE_SUBCLASS(DECLARE_TYPE)
#undef DECLARE_TYPE

  using typename Base::InvokedPrediction;
  using SyntaxParser = Parser<SyntaxParseHandler, Unit>;

 protected:
  using Modifier = TokenStreamShared::Modifier;
  using Position = typename TokenStream::Position;

  using Base::PredictInvoked;
  using Base::PredictUninvoked;

  using Base::alloc_;
  using Base::awaitIsDisallowed;
  using Base::awaitIsKeyword;
  using Base::inParametersOfAsyncFunction;
  using Base::parseGoal;
#if DEBUG
  using Base::checkOptionsCalled_;
#endif
  using Base::checkForUndefinedPrivateFields;
  using Base::errorResult;
  using Base::finishClassBodyScope;
  using Base::finishFunctionScopes;
  using Base::finishLexicalScope;
  using Base::foldConstants_;
  using Base::getFilename;
  using Base::hasValidSimpleStrictParameterNames;
  using Base::isUnexpectedEOF_;
  using Base::nameIsArgumentsOrEval;
  using Base::newDotGeneratorName;
  using Base::newFunctionBox;
  using Base::newName;
  using Base::null;
  using Base::options;
  using Base::pos;
  using Base::propagateFreeNamesAndMarkClosedOverBindings;
  using Base::setLocalStrictMode;
  using Base::stringLiteral;
  using Base::yieldExpressionsSupported;

  using Base::abortIfSyntaxParser;
  using Base::clearAbortedSyntaxParse;
  using Base::disableSyntaxParser;
  using Base::hadAbortedSyntaxParse;

 public:
  // Implement ErrorReportMixin.

  [[nodiscard]] bool computeErrorMetadata(
      ErrorMetadata* err,
      const ErrorReportMixin::ErrorOffset& offset) const override;

  using Base::error;
  using Base::errorAt;
  using Base::errorNoOffset;
  using Base::errorWithNotes;
  using Base::errorWithNotesAt;
  using Base::errorWithNotesNoOffset;
  using Base::strictModeError;
  using Base::strictModeErrorAt;
  using Base::strictModeErrorNoOffset;
  using Base::strictModeErrorWithNotes;
  using Base::strictModeErrorWithNotesAt;
  using Base::strictModeErrorWithNotesNoOffset;
  using Base::warning;
  using Base::warningAt;
  using Base::warningNoOffset;

 public:
  using Base::anyChars;
  using Base::fc_;
  using Base::handler_;
  using Base::noteUsedName;
  using Base::pc_;
  using Base::usedNames_;

 private:
  using Base::checkAndMarkSuperScope;
  using Base::finishFunction;
  using Base::identifierReference;
  using Base::leaveInnerFunction;
  using Base::newInternalDotName;
  using Base::newNewTargetName;
  using Base::newThisName;
  using Base::nextTokenContinuesLetDeclaration;
  using Base::noSubstitutionTaggedTemplate;
  using Base::noteDestructuredPositionalFormalParameter;
  using Base::prefixAccessorName;
  using Base::privateNameReference;
  using Base::processExport;
  using Base::processExportFrom;
  using Base::processImport;
  using Base::setFunctionEndFromCurrentToken;

 private:
  inline FinalParser* asFinalParser();
  inline const FinalParser* asFinalParser() const;

  /*
   * A class for temporarily stashing errors while parsing continues.
   *
   * The ability to stash an error is useful for handling situations where we
   * aren't able to verify that an error has occurred until later in the parse.
   * For instance | ({x=1}) | is always parsed as an object literal with
   * a SyntaxError, however, in the case where it is followed by '=>' we rewind
   * and reparse it as a valid arrow function. Here a PossibleError would be
   * set to 'pending' when the initial SyntaxError was encountered then
   * 'resolved' just before rewinding the parser.
   *
   * There are currently two kinds of PossibleErrors: Expression and
   * Destructuring errors. Expression errors are used to mark a possible
   * syntax error when a grammar production is used in an expression context.
   * For example in |{x = 1}|, we mark the CoverInitializedName |x = 1| as a
   * possible expression error, because CoverInitializedName productions
   * are disallowed when an actual ObjectLiteral is expected.
   * Destructuring errors are used to record possible syntax errors in
   * destructuring contexts. For example in |[...rest, ] = []|, we initially
   * mark the trailing comma after the spread expression as a possible
   * destructuring error, because the ArrayAssignmentPattern grammar
   * production doesn't allow a trailing comma after the rest element.
   *
   * When using PossibleError one should set a pending error at the location
   * where an error occurs. From that point, the error may be resolved
   * (invalidated) or left until the PossibleError is checked.
   *
   * Ex:
   *   PossibleError possibleError(*this);
   *   possibleError.setPendingExpressionErrorAt(pos, JSMSG_BAD_PROP_ID);
   *   // A JSMSG_BAD_PROP_ID ParseError is reported, returns false.
   *   if (!possibleError.checkForExpressionError()) {
   *       return false; // we reach this point with a pending exception
   *   }
   *
   *   PossibleError possibleError(*this);
   *   possibleError.setPendingExpressionErrorAt(pos, JSMSG_BAD_PROP_ID);
   *   // Returns true, no error is reported.
   *   if (!possibleError.checkForDestructuringError()) {
   *       return false; // not reached, no pending exception
   *   }
   *
   *   PossibleError possibleError(*this);
   *   // Returns true, no error is reported.
   *   if (!possibleError.checkForExpressionError()) {
   *       return false; // not reached, no pending exception
   *   }
   */
  class MOZ_STACK_CLASS PossibleError {
   private:
    enum class ErrorKind { Expression, Destructuring, DestructuringWarning };

    enum class ErrorState { None, Pending };

    struct Error {
      ErrorState state_ = ErrorState::None;

      // Error reporting fields.
      uint32_t offset_;
      unsigned errorNumber_;
    };

    GeneralParser<ParseHandler, Unit>& parser_;
    Error exprError_;
    Error destructuringError_;
    Error destructuringWarning_;

    // Returns the error report.
    Error& error(ErrorKind kind);

    // Return true if an error is pending without reporting.
    bool hasError(ErrorKind kind);

    // Resolve any pending error.
    void setResolved(ErrorKind kind);

    // Set a pending error. Only a single error may be set per instance and
    // error kind.
    void setPending(ErrorKind kind, const TokenPos& pos, unsigned errorNumber);

    // If there is a pending error, report it and return false, otherwise
    // return true.
    [[nodiscard]] bool checkForError(ErrorKind kind);

    // Transfer an existing error to another instance.
    void transferErrorTo(ErrorKind kind, PossibleError* other);

   public:
    explicit PossibleError(GeneralParser<ParseHandler, Unit>& parser);

    // Return true if a pending destructuring error is present.
    bool hasPendingDestructuringError();

    // Set a pending destructuring error. Only a single error may be set
    // per instance, i.e. subsequent calls to this method are ignored and
    // won't overwrite the existing pending error.
    void setPendingDestructuringErrorAt(const TokenPos& pos,
                                        unsigned errorNumber);

    // Set a pending destructuring warning. Only a single warning may be
    // set per instance, i.e. subsequent calls to this method are ignored
    // and won't overwrite the existing pending warning.
    void setPendingDestructuringWarningAt(const TokenPos& pos,
                                          unsigned errorNumber);

    // Set a pending expression error. Only a single error may be set per
    // instance, i.e. subsequent calls to this method are ignored and won't
    // overwrite the existing pending error.
    void setPendingExpressionErrorAt(const TokenPos& pos, unsigned errorNumber);

    // If there is a pending destructuring error or warning, report it and
    // return false, otherwise return true. Clears any pending expression
    // error.
    [[nodiscard]] bool checkForDestructuringErrorOrWarning();

    // If there is a pending expression error, report it and return false,
    // otherwise return true. Clears any pending destructuring error or
    // warning.
    [[nodiscard]] bool checkForExpressionError();

    // Pass pending errors between possible error instances. This is useful
    // for extending the lifetime of a pending error beyond the scope of
    // the PossibleError where it was initially set (keeping in mind that
    // PossibleError is a MOZ_STACK_CLASS).
    void transferErrorsTo(PossibleError* other);
  };

 protected:
  SyntaxParser* getSyntaxParser() const {
    return reinterpret_cast<SyntaxParser*>(Base::internalSyntaxParser_);
  }

 public:
  TokenStream tokenStream;

 public:
  GeneralParser(FrontendContext* fc, const JS::ReadOnlyCompileOptions& options,
                const Unit* units, size_t length, bool foldConstants,
                CompilationState& compilationState, SyntaxParser* syntaxParser);

  inline void setAwaitHandling(AwaitHandling awaitHandling);
  inline void setInParametersOfAsyncFunction(bool inParameters);

  /*
   * Parse a top-level JS script.
   */
  ListNodeResult parse();

 private:
  /*
   * Gets the next token and checks if it matches to the given `condition`.
   * If it matches, returns true.
   * If it doesn't match, calls `errorReport` to report the error, and
   * returns false.
   * If other error happens, it returns false but `errorReport` may not be
   * called and other error will be thrown in that case.
   *
   * In any case, the already gotten token is not ungotten.
   *
   * The signature of `condition` is [...](TokenKind actual) -> bool, and
   * the signature of `errorReport` is [...](TokenKind actual).
   */
  template <typename ConditionT, typename ErrorReportT>
  [[nodiscard]] bool mustMatchTokenInternal(ConditionT condition,
                                            ErrorReportT errorReport);

 public:
  /*
   * The following mustMatchToken variants follow the behavior and parameter
   * types of mustMatchTokenInternal above.
   *
   * If modifier is omitted, `SlashIsDiv` is used.
   * If TokenKind is passed instead of `condition`, it checks if the next
   * token is the passed token.
   * If error number is passed instead of `errorReport`, it reports an
   * error with the passed errorNumber.
   */
  [[nodiscard]] bool mustMatchToken(TokenKind expected, JSErrNum errorNumber) {
    return mustMatchTokenInternal(
        [expected](TokenKind actual) { return actual == expected; },
        [this, errorNumber](TokenKind) { this->error(errorNumber); });
  }

  template <typename ConditionT>
  [[nodiscard]] bool mustMatchToken(ConditionT condition,
                                    JSErrNum errorNumber) {
    return mustMatchTokenInternal(condition, [this, errorNumber](TokenKind) {
      this->error(errorNumber);
    });
  }

  template <typename ErrorReportT>
  [[nodiscard]] bool mustMatchToken(TokenKind expected,
                                    ErrorReportT errorReport) {
    return mustMatchTokenInternal(
        [expected](TokenKind actual) { return actual == expected; },
        errorReport);
  }

 private:
  NameNodeResult noSubstitutionUntaggedTemplate();
  ListNodeResult templateLiteral(YieldHandling yieldHandling);
  bool taggedTemplate(YieldHandling yieldHandling, ListNodeType tagArgsList,
                      TokenKind tt);
  bool appendToCallSiteObj(CallSiteNodeType callSiteObj);
  bool addExprAndGetNextTemplStrToken(YieldHandling yieldHandling,
                                      ListNodeType nodeList, TokenKind* ttp);

  inline bool trySyntaxParseInnerFunction(
      FunctionNodeType* funNode, TaggedParserAtomIndex explicitName,
      FunctionFlags flags, uint32_t toStringStart, InHandling inHandling,
      YieldHandling yieldHandling, FunctionSyntaxKind kind,
      GeneratorKind generatorKind, FunctionAsyncKind asyncKind, bool tryAnnexB,
      Directives inheritedDirectives, Directives* newDirectives);

  inline bool skipLazyInnerFunction(FunctionNodeType funNode,
                                    uint32_t toStringStart, bool tryAnnexB);

  void setFunctionStartAtPosition(FunctionBox* funbox, TokenPos pos) const;
  void setFunctionStartAtCurrentToken(FunctionBox* funbox) const;

 public:
  /* Public entry points for parsing. */
  NodeResult statementListItem(YieldHandling yieldHandling,
                               bool canHaveDirectives = false);

  // Parse an inner function given an enclosing ParseContext and a
  // FunctionBox for the inner function.
  [[nodiscard]] FunctionNodeResult innerFunctionForFunctionBox(
      FunctionNodeType funNode, ParseContext* outerpc, FunctionBox* funbox,
      InHandling inHandling, YieldHandling yieldHandling,
      FunctionSyntaxKind kind, Directives* newDirectives);

  // Parse a function's formal parameters and its body assuming its function
  // ParseContext is already on the stack.
  bool functionFormalParametersAndBody(
      InHandling inHandling, YieldHandling yieldHandling,
      FunctionNodeType* funNode, FunctionSyntaxKind kind,
      const mozilla::Maybe<uint32_t>& parameterListEnd = mozilla::Nothing(),
      bool isStandaloneFunction = false);

 private:
  /*
   * JS parsers, from lowest to highest precedence.
   *
   * Each parser must be called during the dynamic scope of a ParseContext
   * object, pointed to by this->pc_.
   *
   * Each returns a parse node tree or null on error.
   */
  FunctionNodeResult functionStmt(
      uint32_t toStringStart, YieldHandling yieldHandling,
      DefaultHandling defaultHandling,
      FunctionAsyncKind asyncKind = FunctionAsyncKind::SyncFunction);
  FunctionNodeResult functionExpr(uint32_t toStringStart,
                                  InvokedPrediction invoked,
                                  FunctionAsyncKind asyncKind);

  NodeResult statement(YieldHandling yieldHandling);
  bool maybeParseDirective(ListNodeType list, Node pn, bool* cont);

  LexicalScopeNodeResult blockStatement(
      YieldHandling yieldHandling,
      unsigned errorNumber = JSMSG_CURLY_IN_COMPOUND);
  BinaryNodeResult doWhileStatement(YieldHandling yieldHandling);
  BinaryNodeResult whileStatement(YieldHandling yieldHandling);

  NodeResult forStatement(YieldHandling yieldHandling);
  bool forHeadStart(YieldHandling yieldHandling, IteratorKind iterKind,
                    ParseNodeKind* forHeadKind, Node* forInitialPart,
                    mozilla::Maybe<ParseContext::Scope>& forLetImpliedScope,
                    Node* forInOrOfExpression);
  NodeResult expressionAfterForInOrOf(ParseNodeKind forHeadKind,
                                      YieldHandling yieldHandling);

  SwitchStatementResult switchStatement(YieldHandling yieldHandling);
  ContinueStatementResult continueStatement(YieldHandling yieldHandling);
  BreakStatementResult breakStatement(YieldHandling yieldHandling);
  UnaryNodeResult returnStatement(YieldHandling yieldHandling);
  BinaryNodeResult withStatement(YieldHandling yieldHandling);
  UnaryNodeResult throwStatement(YieldHandling yieldHandling);
  TernaryNodeResult tryStatement(YieldHandling yieldHandling);
  LexicalScopeNodeResult catchBlockStatement(
      YieldHandling yieldHandling, ParseContext::Scope& catchParamScope);
  DebuggerStatementResult debuggerStatement();

  DeclarationListNodeResult variableStatement(YieldHandling yieldHandling);

  LabeledStatementResult labeledStatement(YieldHandling yieldHandling);
  NodeResult labeledItem(YieldHandling yieldHandling);

  TernaryNodeResult ifStatement(YieldHandling yieldHandling);
  NodeResult consequentOrAlternative(YieldHandling yieldHandling);

  DeclarationListNodeResult lexicalDeclaration(YieldHandling yieldHandling,
                                               DeclarationKind kind);

  NameNodeResult moduleExportName();

  bool withClause(ListNodeType attributesSet);

  BinaryNodeResult importDeclaration();
  NodeResult importDeclarationOrImportExpr(YieldHandling yieldHandling);
  bool namedImports(ListNodeType importSpecSet);
  bool namespaceImport(ListNodeType importSpecSet);

  TaggedParserAtomIndex importedBinding() {
    return bindingIdentifier(YieldIsName);
  }

  BinaryNodeResult exportFrom(uint32_t begin, Node specList);
  BinaryNodeResult exportBatch(uint32_t begin);
  inline bool checkLocalExportNames(ListNodeType node);
  NodeResult exportClause(uint32_t begin);
  UnaryNodeResult exportFunctionDeclaration(
      uint32_t begin, uint32_t toStringStart,
      FunctionAsyncKind asyncKind = FunctionAsyncKind::SyncFunction);
  UnaryNodeResult exportVariableStatement(uint32_t begin);
  UnaryNodeResult exportClassDeclaration(uint32_t begin);
  UnaryNodeResult exportLexicalDeclaration(uint32_t begin,
                                           DeclarationKind kind);
  BinaryNodeResult exportDefaultFunctionDeclaration(
      uint32_t begin, uint32_t toStringStart,
      FunctionAsyncKind asyncKind = FunctionAsyncKind::SyncFunction);
  BinaryNodeResult exportDefaultClassDeclaration(uint32_t begin);
  BinaryNodeResult exportDefaultAssignExpr(uint32_t begin);
  BinaryNodeResult exportDefault(uint32_t begin);
  NodeResult exportDeclaration();

  UnaryNodeResult expressionStatement(
      YieldHandling yieldHandling,
      InvokedPrediction invoked = PredictUninvoked);

  // Declaration parsing.  The main entrypoint is Parser::declarationList,
  // with sub-functionality split out into the remaining methods.

  // |blockScope| may be non-null only when |kind| corresponds to a lexical
  // declaration (that is, ParseNodeKind::LetDecl or ParseNodeKind::ConstDecl).
  //
  // The for* parameters, for normal declarations, should be null/ignored.
  // They should be non-null only when Parser::forHeadStart parses a
  // declaration at the start of a for-loop head.
  //
  // In this case, on success |*forHeadKind| is ParseNodeKind::ForHead,
  // ParseNodeKind::ForIn, or ParseNodeKind::ForOf, corresponding to the three
  // for-loop kinds.  The precise value indicates what was parsed.
  //
  // If parsing recognized a for(;;) loop, the next token is the ';' within
  // the loop-head that separates the init/test parts.
  //
  // Otherwise, for for-in/of loops, the next token is the ')' ending the
  // loop-head.  Additionally, the expression that the loop iterates over was
  // parsed into |*forInOrOfExpression|.
  DeclarationListNodeResult declarationList(
      YieldHandling yieldHandling, ParseNodeKind kind,
      ParseNodeKind* forHeadKind = nullptr,
      Node* forInOrOfExpression = nullptr);

  // The items in a declaration list are either patterns or names, with or
  // without initializers.  These two methods parse a single pattern/name and
  // any associated initializer -- and if parsing an |initialDeclaration|
  // will, if parsing in a for-loop head (as specified by |forHeadKind| being
  // non-null), consume additional tokens up to the closing ')' in a
  // for-in/of loop head, returning the iterated expression in
  // |*forInOrOfExpression|.  (An "initial declaration" is the first
  // declaration in a declaration list: |a| but not |b| in |var a, b|, |{c}|
  // but not |d| in |let {c} = 3, d|.)
  NodeResult declarationPattern(DeclarationKind declKind, TokenKind tt,
                                bool initialDeclaration,
                                YieldHandling yieldHandling,
                                ParseNodeKind* forHeadKind,
                                Node* forInOrOfExpression);
  NodeResult declarationName(DeclarationKind declKind, TokenKind tt,
                             bool initialDeclaration,
                             YieldHandling yieldHandling,
                             ParseNodeKind* forHeadKind,
                             Node* forInOrOfExpression);

  // Having parsed a name (not found in a destructuring pattern) declared by
  // a declaration, with the current token being the '=' separating the name
  // from its initializer, parse and bind that initializer -- and possibly
  // consume trailing in/of and subsequent expression, if so directed by
  // |forHeadKind|.
  AssignmentNodeResult initializerInNameDeclaration(NameNodeType binding,
                                                    DeclarationKind declKind,
                                                    bool initialDeclaration,
                                                    YieldHandling yieldHandling,
                                                    ParseNodeKind* forHeadKind,
                                                    Node* forInOrOfExpression);

  NodeResult expr(InHandling inHandling, YieldHandling yieldHandling,
                  TripledotHandling tripledotHandling,
                  PossibleError* possibleError = nullptr,
                  InvokedPrediction invoked = PredictUninvoked);
  NodeResult assignExpr(InHandling inHandling, YieldHandling yieldHandling,
                        TripledotHandling tripledotHandling,
                        PossibleError* possibleError = nullptr,
                        InvokedPrediction invoked = PredictUninvoked);
  NodeResult assignExprWithoutYieldOrAwait(YieldHandling yieldHandling);
  UnaryNodeResult yieldExpression(InHandling inHandling);
  NodeResult condExpr(InHandling inHandling, YieldHandling yieldHandling,
                      TripledotHandling tripledotHandling,
                      PossibleError* possibleError, InvokedPrediction invoked);
  NodeResult orExpr(InHandling inHandling, YieldHandling yieldHandling,
                    TripledotHandling tripledotHandling,
                    PossibleError* possibleError, InvokedPrediction invoked);
  NodeResult unaryExpr(YieldHandling yieldHandling,
                       TripledotHandling tripledotHandling,
                       PossibleError* possibleError = nullptr,
                       InvokedPrediction invoked = PredictUninvoked,
                       PrivateNameHandling privateNameHandling =
                           PrivateNameHandling::PrivateNameProhibited);
  NodeResult optionalExpr(YieldHandling yieldHandling,
                          TripledotHandling tripledotHandling, TokenKind tt,
                          PossibleError* possibleError = nullptr,
                          InvokedPrediction invoked = PredictUninvoked);
  NodeResult memberExpr(YieldHandling yieldHandling,
                        TripledotHandling tripledotHandling, TokenKind tt,
                        bool allowCallSyntax, PossibleError* possibleError,
                        InvokedPrediction invoked);
  NodeResult decoratorExpr(YieldHandling yieldHandling, TokenKind tt);
  NodeResult primaryExpr(YieldHandling yieldHandling,
                         TripledotHandling tripledotHandling, TokenKind tt,
                         PossibleError* possibleError,
                         InvokedPrediction invoked);
  NodeResult exprInParens(InHandling inHandling, YieldHandling yieldHandling,
                          TripledotHandling tripledotHandling,
                          PossibleError* possibleError = nullptr);

  bool tryNewTarget(NewTargetNodeType* newTarget);

  BinaryNodeResult importExpr(YieldHandling yieldHandling,
                              bool allowCallSyntax);

  FunctionNodeResult methodDefinition(uint32_t toStringStart,
                                      PropertyType propType,
                                      TaggedParserAtomIndex funName);

  /*
   * Additional JS parsers.
   */
  bool functionArguments(YieldHandling yieldHandling, FunctionSyntaxKind kind,
                         FunctionNodeType funNode);

  FunctionNodeResult functionDefinition(
      FunctionNodeType funNode, uint32_t toStringStart, InHandling inHandling,
      YieldHandling yieldHandling, TaggedParserAtomIndex name,
      FunctionSyntaxKind kind, GeneratorKind generatorKind,
      FunctionAsyncKind asyncKind, bool tryAnnexB = false);

  // Parse a function body.  Pass StatementListBody if the body is a list of
  // statements; pass ExpressionBody if the body is a single expression.
  //
  // Don't include opening LeftCurly token when invoking.
  enum FunctionBodyType { StatementListBody, ExpressionBody };
  LexicalScopeNodeResult functionBody(InHandling inHandling,
                                      YieldHandling yieldHandling,
                                      FunctionSyntaxKind kind,
                                      FunctionBodyType type);

  UnaryNodeResult unaryOpExpr(YieldHandling yieldHandling, ParseNodeKind kind,
                              uint32_t begin);

  NodeResult condition(InHandling inHandling, YieldHandling yieldHandling);

  ListNodeResult argumentList(YieldHandling yieldHandling, bool* isSpread,
                              PossibleError* possibleError = nullptr);
  NodeResult destructuringDeclaration(DeclarationKind kind,
                                      YieldHandling yieldHandling,
                                      TokenKind tt);
  NodeResult destructuringDeclarationWithoutYieldOrAwait(
      DeclarationKind kind, YieldHandling yieldHandling, TokenKind tt);

  inline bool checkExportedName(TaggedParserAtomIndex exportName);
  inline bool checkExportedNamesForArrayBinding(ListNodeType array);
  inline bool checkExportedNamesForObjectBinding(ListNodeType obj);
  inline bool checkExportedNamesForDeclaration(Node node);
  inline bool checkExportedNamesForDeclarationList(
      DeclarationListNodeType node);
  inline bool checkExportedNameForFunction(FunctionNodeType funNode);
  inline bool checkExportedNameForClass(ClassNodeType classNode);
  inline bool checkExportedNameForClause(NameNodeType nameNode);

  enum ClassContext { ClassStatement, ClassExpression };
  ClassNodeResult classDefinition(YieldHandling yieldHandling,
                                  ClassContext classContext,
                                  DefaultHandling defaultHandling);

  struct ClassInitializedMembers {
#ifdef ENABLE_DECORATORS
    // Whether a non-static field has decorators or not.
    bool hasInstanceDecorators = false;
#endif

    // The number of instance class fields.
    size_t instanceFields = 0;

    // The number of instance class fields with computed property names.
    size_t instanceFieldKeys = 0;

    // The number of static class fields.
    size_t staticFields = 0;

    // The number of static blocks
    size_t staticBlocks = 0;

    // The number of static class fields with computed property names.
    size_t staticFieldKeys = 0;

    // The number of instance class private methods.
    size_t privateMethods = 0;

    // The number of instance class private accessors.
    size_t privateAccessors = 0;

    bool hasPrivateBrand() const {
      return privateMethods > 0 || privateAccessors > 0;
    }
  };
#ifdef ENABLE_DECORATORS
  ListNodeResult decoratorList(YieldHandling yieldHandling);
#endif
  [[nodiscard]] bool classMember(
      YieldHandling yieldHandling,
      const ParseContext::ClassStatement& classStmt,
      TaggedParserAtomIndex className, uint32_t classStartOffset,
      HasHeritage hasHeritage, ClassInitializedMembers& classInitializedMembers,
      ListNodeType& classMembers, bool* done);
  [[nodiscard]] bool finishClassConstructor(
      const ParseContext::ClassStatement& classStmt,
      TaggedParserAtomIndex className, HasHeritage hasHeritage,
      uint32_t classStartOffset, uint32_t classEndOffset,
      const ClassInitializedMembers& classInitializedMembers,
      ListNodeType& classMembers);

  FunctionNodeResult privateMethodInitializer(
      TokenPos propNamePos, TaggedParserAtomIndex propAtom,
      TaggedParserAtomIndex storedMethodAtom);
  FunctionNodeResult fieldInitializerOpt(
      TokenPos propNamePos, Node name, TaggedParserAtomIndex atom,
      ClassInitializedMembers& classInitializedMembers, bool isStatic,
      HasHeritage hasHeritage);

  FunctionNodeResult synthesizePrivateMethodInitializer(
      TaggedParserAtomIndex propAtom, AccessorType accessorType,
      TokenPos propNamePos);

#ifdef ENABLE_DECORATORS
  FunctionNodeResult synthesizeAddInitializerFunction(
      TaggedParserAtomIndex initializers, YieldHandling yieldHandling);

  ClassMethodResult synthesizeAccessor(
      Node propName, TokenPos propNamePos, TaggedParserAtomIndex propAtom,
      TaggedParserAtomIndex privateStateNameAtom, bool isStatic,
      FunctionSyntaxKind syntaxKind,
      ClassInitializedMembers& classInitializedMembers);

  FunctionNodeResult synthesizeAccessorBody(TaggedParserAtomIndex funNameAtom,
                                            TokenPos propNamePos,
                                            TaggedParserAtomIndex propNameAtom,
                                            FunctionSyntaxKind syntaxKind);
#endif

  FunctionNodeResult staticClassBlock(
      ClassInitializedMembers& classInitializedMembers);

  FunctionNodeResult synthesizeConstructor(TaggedParserAtomIndex className,
                                           TokenPos synthesizedBodyPos,
                                           HasHeritage hasHeritage);

 protected:
  bool synthesizeConstructorBody(TokenPos synthesizedBodyPos,
                                 HasHeritage hasHeritage,
                                 FunctionNodeType funNode, FunctionBox* funbox);

 private:
  bool checkBindingIdentifier(TaggedParserAtomIndex ident, uint32_t offset,
                              YieldHandling yieldHandling,
                              TokenKind hint = TokenKind::Limit);

  TaggedParserAtomIndex labelOrIdentifierReference(YieldHandling yieldHandling);

  TaggedParserAtomIndex labelIdentifier(YieldHandling yieldHandling) {
    return labelOrIdentifierReference(yieldHandling);
  }

  TaggedParserAtomIndex identifierReference(YieldHandling yieldHandling) {
    return labelOrIdentifierReference(yieldHandling);
  }

  bool matchLabel(YieldHandling yieldHandling, TaggedParserAtomIndex* labelOut);

  // Indicate if the next token (tokenized with SlashIsRegExp) is |in| or |of|.
  // If so, consume it.
  bool matchInOrOf(bool* isForInp, bool* isForOfp);

 private:
  bool checkIncDecOperand(Node operand, uint32_t operandOffset);
  bool checkStrictAssignment(Node lhs);

  void reportMissingClosing(unsigned errorNumber, unsigned noteNumber,
                            uint32_t openedPos);

  void reportRedeclarationHelper(TaggedParserAtomIndex& name,
                                 DeclarationKind& prevKind, TokenPos& pos,
                                 uint32_t& prevPos, const unsigned& errorNumber,
                                 const unsigned& noteErrorNumber);

  void reportRedeclaration(TaggedParserAtomIndex name, DeclarationKind prevKind,
                           TokenPos pos, uint32_t prevPos);

  void reportMismatchedPlacement(TaggedParserAtomIndex name,
                                 DeclarationKind prevKind, TokenPos pos,
                                 uint32_t prevPos);

  bool notePositionalFormalParameter(FunctionNodeType funNode,
                                     TaggedParserAtomIndex name,
                                     uint32_t beginPos,
                                     bool disallowDuplicateParams,
                                     bool* duplicatedParam);

  enum PropertyNameContext {
    PropertyNameInLiteral,
    PropertyNameInPattern,
    PropertyNameInClass,
#ifdef ENABLE_RECORD_TUPLE
    PropertyNameInRecord
#endif
  };
  NodeResult propertyName(YieldHandling yieldHandling,
                          PropertyNameContext propertyNameContext,
                          const mozilla::Maybe<DeclarationKind>& maybeDecl,
                          ListNodeType propList,
                          TaggedParserAtomIndex* propAtomOut);
  NodeResult propertyOrMethodName(
      YieldHandling yieldHandling, PropertyNameContext propertyNameContext,
      const mozilla::Maybe<DeclarationKind>& maybeDecl, ListNodeType propList,
      PropertyType* propType, TaggedParserAtomIndex* propAtomOut);
  UnaryNodeResult computedPropertyName(
      YieldHandling yieldHandling,
      const mozilla::Maybe<DeclarationKind>& maybeDecl,
      PropertyNameContext propertyNameContext, ListNodeType literal);
  ListNodeResult arrayInitializer(YieldHandling yieldHandling,
                                  PossibleError* possibleError);
  inline RegExpLiteralResult newRegExp();

  ListNodeResult objectLiteral(YieldHandling yieldHandling,
                               PossibleError* possibleError);

#ifdef ENABLE_RECORD_TUPLE
  ListNodeResult recordLiteral(YieldHandling yieldHandling);
  ListNodeResult tupleLiteral(YieldHandling yieldHandling);
#endif

  BinaryNodeResult bindingInitializer(Node lhs, DeclarationKind kind,
                                      YieldHandling yieldHandling);
  NameNodeResult bindingIdentifier(DeclarationKind kind,
                                   YieldHandling yieldHandling);
  NodeResult bindingIdentifierOrPattern(DeclarationKind kind,
                                        YieldHandling yieldHandling,
                                        TokenKind tt);
  ListNodeResult objectBindingPattern(DeclarationKind kind,
                                      YieldHandling yieldHandling);
  ListNodeResult arrayBindingPattern(DeclarationKind kind,
                                     YieldHandling yieldHandling);

  enum class TargetBehavior {
    PermitAssignmentPattern,
    ForbidAssignmentPattern
  };
  bool checkDestructuringAssignmentTarget(
      Node expr, TokenPos exprPos, PossibleError* exprPossibleError,
      PossibleError* possibleError,
      TargetBehavior behavior = TargetBehavior::PermitAssignmentPattern);
  void checkDestructuringAssignmentName(NameNodeType name, TokenPos namePos,
                                        PossibleError* possibleError);
  bool checkDestructuringAssignmentElement(Node expr, TokenPos exprPos,
                                           PossibleError* exprPossibleError,
                                           PossibleError* possibleError);

  NumericLiteralResult newNumber(const Token& tok) {
    return handler_.newNumber(tok.number(), tok.decimalPoint(), tok.pos);
  }

  inline BigIntLiteralResult newBigInt();

  enum class OptionalKind {
    NonOptional = 0,
    Optional,
  };
  NodeResult memberPropertyAccess(
      Node lhs, OptionalKind optionalKind = OptionalKind::NonOptional);
  NodeResult memberPrivateAccess(
      Node lhs, OptionalKind optionalKind = OptionalKind::NonOptional);
  NodeResult memberElemAccess(
      Node lhs, YieldHandling yieldHandling,
      OptionalKind optionalKind = OptionalKind::NonOptional);
  NodeResult memberSuperCall(Node lhs, YieldHandling yieldHandling);
  NodeResult memberCall(TokenKind tt, Node lhs, YieldHandling yieldHandling,
                        PossibleError* possibleError,
                        OptionalKind optionalKind = OptionalKind::NonOptional);

 protected:
  // Match the current token against the BindingIdentifier production with
  // the given Yield parameter.  If there is no match, report a syntax
  // error.
  TaggedParserAtomIndex bindingIdentifier(YieldHandling yieldHandling);

  bool checkLabelOrIdentifierReference(TaggedParserAtomIndex ident,
                                       uint32_t offset,
                                       YieldHandling yieldHandling,
                                       TokenKind hint = TokenKind::Limit);

  ListNodeResult statementList(YieldHandling yieldHandling);

  [[nodiscard]] FunctionNodeResult innerFunction(
      FunctionNodeType funNode, ParseContext* outerpc,
      TaggedParserAtomIndex explicitName, FunctionFlags flags,
      uint32_t toStringStart, InHandling inHandling,
      YieldHandling yieldHandling, FunctionSyntaxKind kind,
      GeneratorKind generatorKind, FunctionAsyncKind asyncKind, bool tryAnnexB,
      Directives inheritedDirectives, Directives* newDirectives);

  // Implements Automatic Semicolon Insertion.
  //
  // Use this to match `;` in contexts where ASI is allowed. Call this after
  // ruling out all other possibilities except `;`, by peeking ahead if
  // necessary.
  //
  // Unlike most optional Modifiers, this method's `modifier` argument defaults
  // to SlashIsRegExp, since that's by far the most common case: usually an
  // optional semicolon is at the end of a statement or declaration, and the
  // next token could be a RegExp literal beginning a new ExpressionStatement.
  bool matchOrInsertSemicolon(Modifier modifier = TokenStream::SlashIsRegExp);

  bool noteDeclaredName(TaggedParserAtomIndex name, DeclarationKind kind,
                        TokenPos pos, ClosedOver isClosedOver = ClosedOver::No);

  bool noteDeclaredPrivateName(Node nameNode, TaggedParserAtomIndex name,
                               PropertyType propType, FieldPlacement placement,
                               TokenPos pos);

 private:
  inline bool asmJS(ListNodeType list);
};

template <typename Unit>
class MOZ_STACK_CLASS Parser<SyntaxParseHandler, Unit> final
    : public GeneralParser<SyntaxParseHandler, Unit> {
  using Base = GeneralParser<SyntaxParseHandler, Unit>;
  using Node = SyntaxParseHandler::Node;
  using NodeResult = typename SyntaxParseHandler::NodeResult;

#define DECLARE_TYPE(typeName)                               \
  using typeName##Type = SyntaxParseHandler::typeName##Type; \
  using typeName##Result = SyntaxParseHandler::typeName##Result;
  FOR_EACH_PARSENODE_SUBCLASS(DECLARE_TYPE)
#undef DECLARE_TYPE

  using SyntaxParser = Parser<SyntaxParseHandler, Unit>;

  // Numerous Base::* functions have bodies like
  //
  //   return asFinalParser()->func(...);
  //
  // and must be able to call functions here.  Add a friendship relationship
  // so functions here can be hidden when appropriate.
  friend class GeneralParser<SyntaxParseHandler, Unit>;

 public:
  using Base::Base;

  // Inherited types, listed here to have non-dependent names.
  using typename Base::Modifier;
  using typename Base::Position;
  using typename Base::TokenStream;

  // Inherited functions, listed here to have non-dependent names.

 public:
  using Base::anyChars;
  using Base::clearAbortedSyntaxParse;
  using Base::hadAbortedSyntaxParse;
  using Base::innerFunctionForFunctionBox;
  using Base::tokenStream;

 public:
  // ErrorReportMixin.

  using Base::error;
  using Base::errorAt;
  using Base::errorNoOffset;
  using Base::errorWithNotes;
  using Base::errorWithNotesAt;
  using Base::errorWithNotesNoOffset;
  using Base::strictModeError;
  using Base::strictModeErrorAt;
  using Base::strictModeErrorNoOffset;
  using Base::strictModeErrorWithNotes;
  using Base::strictModeErrorWithNotesAt;
  using Base::strictModeErrorWithNotesNoOffset;
  using Base::warning;
  using Base::warningAt;
  using Base::warningNoOffset;

 private:
  using Base::alloc_;
#if DEBUG
  using Base::checkOptionsCalled_;
#endif
  using Base::checkForUndefinedPrivateFields;
  using Base::errorResult;
  using Base::finishFunctionScopes;
  using Base::functionFormalParametersAndBody;
  using Base::handler_;
  using Base::innerFunction;
  using Base::matchOrInsertSemicolon;
  using Base::mustMatchToken;
  using Base::newFunctionBox;
  using Base::newLexicalScopeData;
  using Base::newModuleScopeData;
  using Base::newName;
  using Base::noteDeclaredName;
  using Base::null;
  using Base::options;
  using Base::pc_;
  using Base::pos;
  using Base::propagateFreeNamesAndMarkClosedOverBindings;
  using Base::ss;
  using Base::statementList;
  using Base::stringLiteral;
  using Base::usedNames_;

 private:
  using Base::abortIfSyntaxParser;
  using Base::disableSyntaxParser;

 public:
  // Functions with multiple overloads of different visibility.  We can't
  // |using| the whole thing into existence because of the visibility
  // distinction, so we instead must manually delegate the required overload.

  TaggedParserAtomIndex bindingIdentifier(YieldHandling yieldHandling) {
    return Base::bindingIdentifier(yieldHandling);
  }

  // Functions present in both Parser<ParseHandler, Unit> specializations.

  inline void setAwaitHandling(AwaitHandling awaitHandling);
  inline void setInParametersOfAsyncFunction(bool inParameters);

  RegExpLiteralResult newRegExp();
  BigIntLiteralResult newBigInt();

  // Parse a module.
  ModuleNodeResult moduleBody(ModuleSharedContext* modulesc);

  inline bool checkLocalExportNames(ListNodeType node);
  inline bool checkExportedName(TaggedParserAtomIndex exportName);
  inline bool checkExportedNamesForArrayBinding(ListNodeType array);
  inline bool checkExportedNamesForObjectBinding(ListNodeType obj);
  inline bool checkExportedNamesForDeclaration(Node node);
  inline bool checkExportedNamesForDeclarationList(
      DeclarationListNodeType node);
  inline bool checkExportedNameForFunction(FunctionNodeType funNode);
  inline bool checkExportedNameForClass(ClassNodeType classNode);
  inline bool checkExportedNameForClause(NameNodeType nameNode);

  bool trySyntaxParseInnerFunction(
      FunctionNodeType* funNode, TaggedParserAtomIndex explicitName,
      FunctionFlags flags, uint32_t toStringStart, InHandling inHandling,
      YieldHandling yieldHandling, FunctionSyntaxKind kind,
      GeneratorKind generatorKind, FunctionAsyncKind asyncKind, bool tryAnnexB,
      Directives inheritedDirectives, Directives* newDirectives);

  bool skipLazyInnerFunction(FunctionNodeType funNode, uint32_t toStringStart,
                             bool tryAnnexB);

  bool asmJS(ListNodeType list);

  // Functions present only in Parser<SyntaxParseHandler, Unit>.
};

template <typename Unit>
class MOZ_STACK_CLASS Parser<FullParseHandler, Unit> final
    : public GeneralParser<FullParseHandler, Unit> {
  using Base = GeneralParser<FullParseHandler, Unit>;
  using Node = FullParseHandler::Node;
  using NodeResult = typename FullParseHandler::NodeResult;

#define DECLARE_TYPE(typeName)                             \
  using typeName##Type = FullParseHandler::typeName##Type; \
  using typeName##Result = FullParseHandler::typeName##Result;
  FOR_EACH_PARSENODE_SUBCLASS(DECLARE_TYPE)
#undef DECLARE_TYPE

  using SyntaxParser = Parser<SyntaxParseHandler, Unit>;

  // Numerous Base::* functions have bodies like
  //
  //   return asFinalParser()->func(...);
  //
  // and must be able to call functions here.  Add a friendship relationship
  // so functions here can be hidden when appropriate.
  friend class GeneralParser<FullParseHandler, Unit>;

 public:
  using Base::Base;

  // Inherited types, listed here to have non-dependent names.
  using typename Base::Modifier;
  using typename Base::Position;
  using typename Base::TokenStream;

  // Inherited functions, listed here to have non-dependent names.

 public:
  using Base::anyChars;
  using Base::clearAbortedSyntaxParse;
  using Base::functionFormalParametersAndBody;
  using Base::hadAbortedSyntaxParse;
  using Base::handler_;
  using Base::newFunctionBox;
  using Base::options;
  using Base::pc_;
  using Base::pos;
  using Base::ss;
  using Base::tokenStream;

 public:
  // ErrorReportMixin.

  using Base::error;
  using Base::errorAt;
  using Base::errorNoOffset;
  using Base::errorWithNotes;
  using Base::errorWithNotesAt;
  using Base::errorWithNotesNoOffset;
  using Base::strictModeError;
  using Base::strictModeErrorAt;
  using Base::strictModeErrorNoOffset;
  using Base::strictModeErrorWithNotes;
  using Base::strictModeErrorWithNotesAt;
  using Base::strictModeErrorWithNotesNoOffset;
  using Base::warning;
  using Base::warningAt;
  using Base::warningNoOffset;

 private:
  using Base::alloc_;
  using Base::checkLabelOrIdentifierReference;
#if DEBUG
  using Base::checkOptionsCalled_;
#endif
  using Base::checkForUndefinedPrivateFields;
  using Base::errorResult;
  using Base::fc_;
  using Base::finishClassBodyScope;
  using Base::finishFunctionScopes;
  using Base::finishLexicalScope;
  using Base::innerFunction;
  using Base::innerFunctionForFunctionBox;
  using Base::matchOrInsertSemicolon;
  using Base::mustMatchToken;
  using Base::newEvalScopeData;
  using Base::newFunctionScopeData;
  using Base::newGlobalScopeData;
  using Base::newLexicalScopeData;
  using Base::newModuleScopeData;
  using Base::newName;
  using Base::newVarScopeData;
  using Base::noteDeclaredName;
  using Base::noteUsedName;
  using Base::null;
  using Base::propagateFreeNamesAndMarkClosedOverBindings;
  using Base::statementList;
  using Base::stringLiteral;
  using Base::usedNames_;

  using Base::abortIfSyntaxParser;
  using Base::disableSyntaxParser;
  using Base::getSyntaxParser;

 public:
  // Functions with multiple overloads of different visibility.  We can't
  // |using| the whole thing into existence because of the visibility
  // distinction, so we instead must manually delegate the required overload.

  TaggedParserAtomIndex bindingIdentifier(YieldHandling yieldHandling) {
    return Base::bindingIdentifier(yieldHandling);
  }

  // Functions present in both Parser<ParseHandler, Unit> specializations.

  friend class AutoAwaitIsKeyword<SyntaxParseHandler, Unit>;
  inline void setAwaitHandling(AwaitHandling awaitHandling);

  friend class AutoInParametersOfAsyncFunction<SyntaxParseHandler, Unit>;
  inline void setInParametersOfAsyncFunction(bool inParameters);

  RegExpLiteralResult newRegExp();
  BigIntLiteralResult newBigInt();

  // Parse a module.
  ModuleNodeResult moduleBody(ModuleSharedContext* modulesc);

  bool checkLocalExportNames(ListNodeType node);
  bool checkExportedName(TaggedParserAtomIndex exportName);
  bool checkExportedNamesForArrayBinding(ListNodeType array);
  bool checkExportedNamesForObjectBinding(ListNodeType obj);
  bool checkExportedNamesForDeclaration(Node node);
  bool checkExportedNamesForDeclarationList(DeclarationListNodeType node);
  bool checkExportedNameForFunction(FunctionNodeType funNode);
  bool checkExportedNameForClass(ClassNodeType classNode);
  inline bool checkExportedNameForClause(NameNodeType nameNode);

  bool trySyntaxParseInnerFunction(
      FunctionNodeType* funNode, TaggedParserAtomIndex explicitName,
      FunctionFlags flags, uint32_t toStringStart, InHandling inHandling,
      YieldHandling yieldHandling, FunctionSyntaxKind kind,
      GeneratorKind generatorKind, FunctionAsyncKind asyncKind, bool tryAnnexB,
      Directives inheritedDirectives, Directives* newDirectives);

  [[nodiscard]] bool advancePastSyntaxParsedFunction(
      SyntaxParser* syntaxParser);

  bool skipLazyInnerFunction(FunctionNodeType funNode, uint32_t toStringStart,
                             bool tryAnnexB);

  // Functions present only in Parser<FullParseHandler, Unit>.

  // Parse the body of an eval.
  //
  // Eval scripts are distinguished from global scripts in that in ES6, per
  // 18.2.1.1 steps 9 and 10, all eval scripts are executed under a fresh
  // lexical scope.
  LexicalScopeNodeResult evalBody(EvalSharedContext* evalsc);

  // Parse a function, given only its arguments and body. Used for lazily
  // parsed functions.
  FunctionNodeResult standaloneLazyFunction(CompilationInput& input,
                                            uint32_t toStringStart, bool strict,
                                            GeneratorKind generatorKind,
                                            FunctionAsyncKind asyncKind);

  // Parse a function, used for the Function, GeneratorFunction, and
  // AsyncFunction constructors.
  FunctionNodeResult standaloneFunction(
      const mozilla::Maybe<uint32_t>& parameterListEnd,
      FunctionSyntaxKind syntaxKind, GeneratorKind generatorKind,
      FunctionAsyncKind asyncKind, Directives inheritedDirectives,
      Directives* newDirectives);

  bool checkStatementsEOF();

  // Parse the body of a global script.
  ListNodeResult globalBody(GlobalSharedContext* globalsc);

  bool checkLocalExportName(TaggedParserAtomIndex ident, uint32_t offset) {
    return checkLabelOrIdentifierReference(ident, offset, YieldIsName);
  }

  bool asmJS(ListNodeType list);
};

template <class Parser>
/* static */ inline const TokenStreamAnyChars&
ParserAnyCharsAccess<Parser>::anyChars(const GeneralTokenStreamChars* ts) {
  // The structure we're walking through looks like this:
  //
  //   struct ParserBase
  //   {
  //       ...;
  //       TokenStreamAnyChars anyChars;
  //       ...;
  //   };
  //   struct Parser : <class that ultimately inherits from ParserBase>
  //   {
  //       ...;
  //       TokenStreamSpecific tokenStream;
  //       ...;
  //   };
  //
  // We're passed a GeneralTokenStreamChars* (this being a base class of
  // Parser::tokenStream).  We cast that pointer to a TokenStreamSpecific*,
  // then translate that to the enclosing Parser*, then return the |anyChars|
  // member within.

  static_assert(std::is_base_of_v<GeneralTokenStreamChars, TokenStreamSpecific>,
                "the static_cast<> below assumes a base-class relationship");
  const auto* tss = static_cast<const TokenStreamSpecific*>(ts);

  auto tssAddr = reinterpret_cast<uintptr_t>(tss);

  using ActualTokenStreamType = decltype(std::declval<Parser>().tokenStream);
  static_assert(std::is_same_v<ActualTokenStreamType, TokenStreamSpecific>,
                "Parser::tokenStream must have type TokenStreamSpecific");

  uintptr_t parserAddr = tssAddr - offsetof(Parser, tokenStream);

  return reinterpret_cast<const Parser*>(parserAddr)->anyChars;
}

template <class Parser>
/* static */ inline TokenStreamAnyChars& ParserAnyCharsAccess<Parser>::anyChars(
    GeneralTokenStreamChars* ts) {
  const TokenStreamAnyChars& anyCharsConst =
      anyChars(const_cast<const GeneralTokenStreamChars*>(ts));

  return const_cast<TokenStreamAnyChars&>(anyCharsConst);
}

template <class ParseHandler, typename Unit>
class MOZ_STACK_CLASS AutoAwaitIsKeyword {
  using GeneralParser = frontend::GeneralParser<ParseHandler, Unit>;

 private:
  GeneralParser* parser_;
  AwaitHandling oldAwaitHandling_;

 public:
  AutoAwaitIsKeyword(GeneralParser* parser, AwaitHandling awaitHandling) {
    parser_ = parser;
    oldAwaitHandling_ = static_cast<AwaitHandling>(parser_->awaitHandling_);

    // 'await' is always a keyword in module contexts, so we don't modify
    // the state when the original handling is AwaitIsModuleKeyword.
    if (oldAwaitHandling_ != AwaitIsModuleKeyword) {
      parser_->setAwaitHandling(awaitHandling);
    }
  }

  ~AutoAwaitIsKeyword() { parser_->setAwaitHandling(oldAwaitHandling_); }
};

template <class ParseHandler, typename Unit>
class MOZ_STACK_CLASS AutoInParametersOfAsyncFunction {
  using GeneralParser = frontend::GeneralParser<ParseHandler, Unit>;

 private:
  GeneralParser* parser_;
  bool oldInParametersOfAsyncFunction_;

 public:
  AutoInParametersOfAsyncFunction(GeneralParser* parser, bool inParameters) {
    parser_ = parser;
    oldInParametersOfAsyncFunction_ = parser_->inParametersOfAsyncFunction_;
    parser_->setInParametersOfAsyncFunction(inParameters);
  }

  ~AutoInParametersOfAsyncFunction() {
    parser_->setInParametersOfAsyncFunction(oldInParametersOfAsyncFunction_);
  }
};

GlobalScope::ParserData* NewEmptyGlobalScopeData(FrontendContext* fc,
                                                 LifoAlloc& alloc,
                                                 uint32_t numBindings);

VarScope::ParserData* NewEmptyVarScopeData(FrontendContext* fc,
                                           LifoAlloc& alloc,
                                           uint32_t numBindings);

LexicalScope::ParserData* NewEmptyLexicalScopeData(FrontendContext* fc,
                                                   LifoAlloc& alloc,
                                                   uint32_t numBindings);

FunctionScope::ParserData* NewEmptyFunctionScopeData(FrontendContext* fc,
                                                     LifoAlloc& alloc,
                                                     uint32_t numBindings);

bool FunctionScopeHasClosedOverBindings(ParseContext* pc);
bool LexicalScopeHasClosedOverBindings(ParseContext* pc,
                                       ParseContext::Scope& scope);

} /* namespace frontend */
} /* namespace js */

#endif /* frontend_Parser_h */
