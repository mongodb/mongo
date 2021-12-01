/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* JS parser. */

#ifndef frontend_Parser_h
#define frontend_Parser_h

/*
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
 * == ParserBase → JS::AutoGCRooter, StrictModeGetter ==
 *
 * ParserBase is the base parser class, shared by all parsers of all character
 * types and parse-handling behavior.  It stores everything character- and
 * handler-agnostic.
 *
 * ParserBase's most important field is the parser's token stream's
 * |TokenStreamAnyChars| component, for all tokenizing aspects that are
 * character-type-agnostic.  The character-type-sensitive components residing
 * in |TokenStreamSpecific| (see the comment near the top of TokenStream.h)
 * live elsewhere in this hierarchy.  These separate locations are the reason
 * for the |AnyCharsAccess| template parameter to |TokenStreamChars| and
 * |TokenStreamSpecific|.
 *
 * Of particular note: making ParserBase inherit JS::AutoGCRooter (rather than
 * placing it under one of the more-derived parser classes) means that all
 * parsers can be traced using the same AutoGCRooter mechanism: it's not
 * necessary to have separate tracing functionality for syntax/full parsers or
 * parsers of different character types.
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
 * == GeneralParser<ParseHandler, CharT> → PerHandlerParser<ParseHandler> ==
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
 * be instantiated once per CharT -- but if exactly equivalent code would be
 * generated (because PerHandlerParser functions have no awareness of CharT),
 * it's risky to *depend* upon the compiler coalescing the instantiations into
 * one in the final binary.  PerHandlerParser guarantees no duplication.
 *
 * == Parser<ParseHandler, CharT> final → GeneralParser<ParseHandler, CharT> ==
 *
 * The final (pun intended) axis of complexity lies in Parser.
 *
 * Some functionality depends on character type, yet also is defined in
 * significantly different form in full and syntax parsing.  For example,
 * attempting to parse the source text of a module will do so in full parsing
 * but immediately fail in syntax parsing -- so the former is a mess'o'code
 * while the latter is effectively |return null();|.  Such functionality is
 * defined in Parser<SyntaxParseHandler or FullParseHandler, CharT> as
 * appropriate.
 *
 * There's a crucial distinction between GeneralParser and Parser, that
 * explains why both must exist (despite taking exactly the same template
 * parameters, and despite GeneralParser and Parser existing in a one-to-one
 * relationship).  GeneralParser is one unspecialized template class:
 *
 *   template<class ParseHandler, typename CharT>
 *   class GeneralParser : ...
 *   {
 *     ...parsing functions...
 *   };
 *
 * but Parser is one undefined template class with two separate
 * specializations:
 *
 *   // Declare, but do not define.
 *   template<class ParseHandler, typename CharT> class Parser;
 *
 *   // Define a syntax-parsing specialization.
 *   template<typename CharT>
 *   class Parser<SyntaxParseHandler, CharT> final
 *     : public GeneralParser<SyntaxParseHandler, CharT>
 *   {
 *     ...parsing functions...
 *   };
 *
 *   // Define a full-parsing specialization.
 *   template<typename CharT>
 *   class Parser<SyntaxParseHandler, CharT> final
 *     : public GeneralParser<SyntaxParseHandler, CharT>
 *   {
 *     ...parsing functions...
 *   };
 *
 * This odd distinction is necessary because C++ unfortunately doesn't allow
 * partial function specialization:
 *
 *   // BAD: You can only specialize a template function if you specify *every*
 *   //      template parameter, i.e. ParseHandler *and* CharT.
 *   template<typename CharT>
 *   void
 *   GeneralParser<SyntaxParseHandler, CharT>::foo() {}
 *
 * But if you specialize Parser *as a class*, then this is allowed:
 *
 *   template<typename CharT>
 *   void
 *   Parser<SyntaxParseHandler, CharT>::foo() {}
 *
 *   template<typename CharT>
 *   void
 *   Parser<FullParseHandler, CharT>::foo() {}
 *
 * because the only template parameter on the function is CharT -- and so all
 * template parameters *are* varying, not a strict subset of them.
 *
 * So -- any parsing functionality that is differently defined for different
 * ParseHandlers, *but* is defined textually identically for different CharT
 * (even if different code ends up generated for them by the compiler), should
 * reside in Parser.
 */

#include "mozilla/Array.h"
#include "mozilla/Maybe.h"
#include "mozilla/TypeTraits.h"

#include "jspubtd.h"

#include "ds/Nestable.h"
#include "frontend/BytecodeCompiler.h"
#include "frontend/FullParseHandler.h"
#include "frontend/LanguageExtensions.h"
#include "frontend/NameAnalysisTypes.h"
#include "frontend/NameCollections.h"
#include "frontend/ParseContext.h"
#include "frontend/SharedContext.h"
#include "frontend/SyntaxParseHandler.h"
#include "frontend/TokenStream.h"
#include "vm/Iteration.h"

namespace js {

class ModuleObject;

namespace frontend {

class ParserBase;

template <class ParseHandler, typename CharT>
class GeneralParser;

class SourceParseContext: public ParseContext
{
public:
    template<typename ParseHandler, typename CharT>
    SourceParseContext(GeneralParser<ParseHandler, CharT>* prs, SharedContext* sc,
                       Directives* newDirectives)
      : ParseContext(prs->context, prs->pc, sc, prs->anyChars, prs->usedNames, newDirectives,
                     mozilla::IsSame<ParseHandler, FullParseHandler>::value)
    { }
};

template <typename T>
inline T&
ParseContext::Statement::as()
{
    MOZ_ASSERT(is<T>());
    return static_cast<T&>(*this);
}

inline ParseContext::Scope::BindingIter
ParseContext::Scope::bindings(ParseContext* pc)
{
    // In function scopes with parameter expressions, function special names
    // (like '.this') are declared as vars in the function scope, despite its
    // not being the var scope.
    return BindingIter(*this, pc->varScope_ == this || pc->functionScope_.ptrOr(nullptr) == this);
}

inline
Directives::Directives(ParseContext* parent)
  : strict_(parent->sc()->strict()),
    asmJS_(parent->useAsmOrInsideUseAsm())
{}

enum VarContext { HoistVars, DontHoistVars };
enum PropListType { ObjectLiteral, ClassBody, DerivedClassBody };
enum class PropertyType {
    Normal,
    Shorthand,
    CoverInitializedName,
    Getter,
    GetterNoExpressionClosure,
    Setter,
    SetterNoExpressionClosure,
    Method,
    GeneratorMethod,
    AsyncMethod,
    AsyncGeneratorMethod,
    Constructor,
    DerivedConstructor
};

enum AwaitHandling : uint8_t { AwaitIsName, AwaitIsKeyword, AwaitIsModuleKeyword };

template <class ParseHandler, typename CharT>
class AutoAwaitIsKeyword;

class ParserBase
  : public StrictModeGetter,
    private JS::AutoGCRooter
{
  private:
    ParserBase* thisForCtor() { return this; }

    // This is needed to cast a parser to JS::AutoGCRooter.
    friend void js::frontend::TraceParser(JSTracer* trc, JS::AutoGCRooter* parser);

  public:
    JSContext* const context;

    LifoAlloc& alloc;

    TokenStreamAnyChars anyChars;
    LifoAlloc::Mark tempPoolMark;

    /* list of parsed objects for GC tracing */
    ObjectBox* traceListHead;

    /* innermost parse context (stack-allocated) */
    ParseContext* pc;

    // For tracking used names in this parsing session.
    UsedNameTracker& usedNames;

    ScriptSource*       ss;

    /* Root atoms and objects allocated for the parsed tree. */
    AutoKeepAtoms       keepAtoms;

    /* Perform constant-folding; must be true when interfacing with the emitter. */
    const bool          foldConstants:1;

  protected:
#if DEBUG
    /* Our fallible 'checkOptions' member function has been called. */
    bool checkOptionsCalled:1;
#endif

    /* Unexpected end of input, i.e. Eof not at top-level. */
    bool isUnexpectedEOF_:1;

    /* AwaitHandling */ uint8_t awaitHandling_:2;

  public:
    bool awaitIsKeyword() const {
      return awaitHandling_ != AwaitIsName;
    }

    template<class, typename> friend class AutoAwaitIsKeyword;

    ParserBase(JSContext* cx, LifoAlloc& alloc, const ReadOnlyCompileOptions& options,
               bool foldConstants, UsedNameTracker& usedNames);
    ~ParserBase();

    bool checkOptions();

    void trace(JSTracer* trc);

    const char* getFilename() const { return anyChars.getFilename(); }
    TokenPos pos() const { return anyChars.currentToken().pos; }

    // Determine whether |yield| is a valid name in the current context.
    bool yieldExpressionsSupported() const {
        return pc->isGenerator();
    }

    virtual bool strictMode() override { return pc->sc()->strict(); }
    bool setLocalStrictMode(bool strict) {
        MOZ_ASSERT(anyChars.debugHasNoLookahead());
        return pc->sc()->setLocalStrictMode(strict);
    }

    const ReadOnlyCompileOptions& options() const {
        return anyChars.options();
    }

    bool isUnexpectedEOF() const { return isUnexpectedEOF_; }

    MOZ_MUST_USE bool warningNoOffset(unsigned errorNumber, ...);
    void errorNoOffset(unsigned errorNumber, ...);

    bool isValidStrictBinding(PropertyName* name);

    void addTelemetry(DeprecatedLanguageExtension e);

    bool hasValidSimpleStrictParameterNames();

    bool allowExpressionClosures() const {
        return options().expressionClosuresOption;
    }
    /*
     * Create a new function object given a name (which is optional if this is
     * a function expression).
     */
    JSFunction* newFunction(HandleAtom atom, FunctionSyntaxKind kind,
                            GeneratorKind generatorKind, FunctionAsyncKind asyncKind,
                            HandleObject proto);

    // A Parser::Mark is the extension of the LifoAlloc::Mark to the entire
    // Parser's state. Note: clients must still take care that any ParseContext
    // that points into released ParseNodes is destroyed.
    class Mark
    {
        friend class ParserBase;
        LifoAlloc::Mark mark;
        ObjectBox* traceListHead;
    };
    Mark mark() const {
        Mark m;
        m.mark = alloc.mark();
        m.traceListHead = traceListHead;
        return m;
    }
    void release(Mark m) {
        alloc.release(m.mark);
        traceListHead = m.traceListHead;
    }

    ObjectBox* newObjectBox(JSObject* obj);

    mozilla::Maybe<GlobalScope::Data*> newGlobalScopeData(ParseContext::Scope& scope);
    mozilla::Maybe<ModuleScope::Data*> newModuleScopeData(ParseContext::Scope& scope);
    mozilla::Maybe<EvalScope::Data*> newEvalScopeData(ParseContext::Scope& scope);
    mozilla::Maybe<FunctionScope::Data*> newFunctionScopeData(ParseContext::Scope& scope,
                                                              bool hasParameterExprs);
    mozilla::Maybe<VarScope::Data*> newVarScopeData(ParseContext::Scope& scope);
    mozilla::Maybe<LexicalScope::Data*> newLexicalScopeData(ParseContext::Scope& scope);

  protected:
    enum InvokedPrediction { PredictUninvoked = false, PredictInvoked = true };
    enum ForInitLocation { InForInit, NotInForInit };

    // While on a |let| Name token, examine |next| (which must already be
    // gotten).  Indicate whether |next|, the next token already gotten with
    // modifier TokenStream::None, continues a LexicalDeclaration.
    bool nextTokenContinuesLetDeclaration(TokenKind next);

    bool noteUsedNameInternal(HandlePropertyName name);
    bool hasUsedName(HandlePropertyName name);
    bool hasUsedFunctionSpecialName(HandlePropertyName name);

    bool checkAndMarkSuperScope();

    bool declareDotGeneratorName();

    bool leaveInnerFunction(ParseContext* outerpc);

    JSAtom* prefixAccessorName(PropertyType propType, HandleAtom propAtom);
};

inline
ParseContext::Scope::Scope(ParserBase* parser)
  : Nestable<Scope>(&parser->pc->innermostScope_),
    declared_(parser->context->frontendCollectionPool()),
    possibleAnnexBFunctionBoxes_(parser->context->frontendCollectionPool()),
    id_(parser->usedNames.nextScopeId())
{ }

inline
ParseContext::Scope::Scope(JSContext* cx, ParseContext* pc, UsedNameTracker& usedNames)
  : Nestable<Scope>(&pc->innermostScope_),
    declared_(cx->frontendCollectionPool()),
    possibleAnnexBFunctionBoxes_(cx->frontendCollectionPool()),
    id_(usedNames.nextScopeId())
{ }

inline
ParseContext::VarScope::VarScope(ParserBase* parser)
  : Scope(parser)
{
    useAsVarScope(parser->pc);
}

inline
ParseContext::VarScope::VarScope(JSContext* cx, ParseContext* pc, UsedNameTracker& usedNames)
  : Scope(cx, pc, usedNames)
{
    useAsVarScope(pc);
}

enum FunctionCallBehavior {
    PermitAssignmentToFunctionCalls,
    ForbidAssignmentToFunctionCalls
};

template <class ParseHandler>
class PerHandlerParser
  : public ParserBase
{
  private:
    using Node = typename ParseHandler::Node;

  protected:
    /* State specific to the kind of parse being performed. */
    ParseHandler handler;

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
    // |internalSyntaxParser_| is really a |Parser<SyntaxParseHandler, CharT>*|
    // where |CharT| varies per |Parser<ParseHandler, CharT>|.  But this
    // template class doesn't have access to |CharT|, so we store a |void*|
    // here, then intermediate all access to this field through accessors in
    // |GeneralParser<ParseHandler, CharT>| that impose the real type on this
    // field.
    void* internalSyntaxParser_;

  protected:
    PerHandlerParser(JSContext* cx, LifoAlloc& alloc, const ReadOnlyCompileOptions& options,
                     bool foldConstants, UsedNameTracker& usedNames,
                     LazyScript* lazyOuterFunction);

    static Node null() { return ParseHandler::null(); }

    Node stringLiteral();

    const char* nameIsArgumentsOrEval(Node node);

    bool noteDestructuredPositionalFormalParameter(Node fn, Node destruct);

    bool noteUsedName(HandlePropertyName name) {
        // If the we are delazifying, the LazyScript already has all the
        // closed-over info for bindings and there's no need to track used
        // names.
        if (handler.canSkipLazyClosedOverBindings())
            return true;

        return ParserBase::noteUsedNameInternal(name);
    }

    // Required on Scope exit.
    bool propagateFreeNamesAndMarkClosedOverBindings(ParseContext::Scope& scope);

    bool finishFunctionScopes(bool isStandaloneFunction);
    Node finishLexicalScope(ParseContext::Scope& scope, Node body);
    bool finishFunction(bool isStandaloneFunction = false);

    bool declareFunctionThis();
    bool declareFunctionArgumentsObject();

    inline Node newName(PropertyName* name);
    inline Node newName(PropertyName* name, TokenPos pos);

    Node newInternalDotName(HandlePropertyName name);
    Node newThisName();
    Node newDotGeneratorName();

    Node identifierReference(Handle<PropertyName*> name);

    Node noSubstitutionTaggedTemplate();

    inline bool processExport(Node node);
    inline bool processExportFrom(Node node);

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
    void prepareNodeForMutation(Node node) { handler.prepareNodeForMutation(node); }
    void freeTree(Node node) { handler.freeTree(node); }

    bool isValidSimpleAssignmentTarget(Node node,
                                       FunctionCallBehavior behavior = ForbidAssignmentToFunctionCalls);

    Node newPropertyAccess(Node expr, PropertyName* key, uint32_t end) {
        return handler.newPropertyAccess(expr, key, end);
    }

    FunctionBox* newFunctionBox(Node fn, JSFunction* fun, uint32_t toStringStart,
                                Directives directives, GeneratorKind generatorKind,
                                FunctionAsyncKind asyncKind);
};

#define ABORTED_SYNTAX_PARSE_SENTINEL reinterpret_cast<void*>(0x1)

template<>
inline void
PerHandlerParser<SyntaxParseHandler>::disableSyntaxParser()
{
}

template<>
inline bool
PerHandlerParser<SyntaxParseHandler>::abortIfSyntaxParser()
{
    internalSyntaxParser_ = ABORTED_SYNTAX_PARSE_SENTINEL;
    return false;
}

template<>
inline bool
PerHandlerParser<SyntaxParseHandler>::hadAbortedSyntaxParse()
{
    return internalSyntaxParser_ == ABORTED_SYNTAX_PARSE_SENTINEL;
}

template<>
inline void
PerHandlerParser<SyntaxParseHandler>::clearAbortedSyntaxParse()
{
    internalSyntaxParser_ = nullptr;
}

#undef ABORTED_SYNTAX_PARSE_SENTINEL

// Disable syntax parsing of all future inner functions during this
// full-parse.
template<>
inline void
PerHandlerParser<FullParseHandler>::disableSyntaxParser()
{
    internalSyntaxParser_ = nullptr;
}

template<>
inline bool
PerHandlerParser<FullParseHandler>::abortIfSyntaxParser()
{
    disableSyntaxParser();
    return true;
}

template<>
inline bool
PerHandlerParser<FullParseHandler>::hadAbortedSyntaxParse()
{
    return false;
}

template<>
inline void
PerHandlerParser<FullParseHandler>::clearAbortedSyntaxParse()
{
}

enum class ExpressionClosure { Allowed, Forbidden };

template<class Parser>
class ParserAnyCharsAccess
{
  public:
    using TokenStreamSpecific = typename Parser::TokenStream;
    using GeneralTokenStreamChars = typename TokenStreamSpecific::GeneralCharsBase;

    static inline TokenStreamAnyChars& anyChars(GeneralTokenStreamChars* ts);
    static inline const TokenStreamAnyChars& anyChars(const GeneralTokenStreamChars* ts);
};

// Specify a value for an ES6 grammar parametrization.  We have no enum for
// [Return] because its behavior is exactly equivalent to checking whether
// we're in a function box -- easier and simpler than passing an extra
// parameter everywhere.
enum YieldHandling { YieldIsName, YieldIsKeyword };
enum InHandling { InAllowed, InProhibited };
enum DefaultHandling { NameRequired, AllowDefaultName };
enum TripledotHandling { TripledotAllowed, TripledotProhibited };

template <class ParseHandler, typename CharT>
class Parser;

template <class ParseHandler, typename CharT>
class GeneralParser
  : public PerHandlerParser<ParseHandler>
{
  public:
    using TokenStream = TokenStreamSpecific<CharT, ParserAnyCharsAccess<GeneralParser>>;

  private:
    using Base = PerHandlerParser<ParseHandler>;
    using FinalParser = Parser<ParseHandler, CharT>;
    using Node = typename ParseHandler::Node;
    using typename Base::InvokedPrediction;
    using SyntaxParser = Parser<SyntaxParseHandler, CharT>;

  protected:
    using Modifier = TokenStreamShared::Modifier;
    using Position = typename TokenStream::Position;

    using Base::PredictUninvoked;
    using Base::PredictInvoked;

    using Base::alloc;
    using Base::awaitIsKeyword;
#if DEBUG
    using Base::checkOptionsCalled;
#endif
    using Base::finishFunctionScopes;
    using Base::finishLexicalScope;
    using Base::foldConstants;
    using Base::getFilename;
    using Base::hasUsedFunctionSpecialName;
    using Base::hasValidSimpleStrictParameterNames;
    using Base::isUnexpectedEOF_;
    using Base::keepAtoms;
    using Base::nameIsArgumentsOrEval;
    using Base::newFunction;
    using Base::newFunctionBox;
    using Base::newName;
    using Base::null;
    using Base::options;
    using Base::pos;
    using Base::propagateFreeNamesAndMarkClosedOverBindings;
    using Base::setLocalStrictMode;
    using Base::stringLiteral;
    using Base::traceListHead;
    using Base::yieldExpressionsSupported;

    using Base::disableSyntaxParser;
    using Base::abortIfSyntaxParser;
    using Base::hadAbortedSyntaxParse;
    using Base::clearAbortedSyntaxParse;

  public:
    using Base::anyChars;
    using Base::context;
    using Base::handler;
    using Base::isValidSimpleAssignmentTarget;
    using Base::pc;
    using Base::usedNames;
    using Base::allowExpressionClosures;

  private:
    using Base::checkAndMarkSuperScope;
    using Base::declareDotGeneratorName;
    using Base::declareFunctionArgumentsObject;
    using Base::declareFunctionThis;
    using Base::finishFunction;
    using Base::hasUsedName;
    using Base::identifierReference;
    using Base::leaveInnerFunction;
    using Base::newDotGeneratorName;
    using Base::newInternalDotName;
    using Base::newThisName;
    using Base::nextTokenContinuesLetDeclaration;
    using Base::noSubstitutionTaggedTemplate;
    using Base::noteDestructuredPositionalFormalParameter;
    using Base::noteUsedName;
    using Base::prefixAccessorName;
    using Base::processExport;
    using Base::processExportFrom;

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
     * set to 'pending' when the initial SyntaxError was encountered then 'resolved'
     * just before rewinding the parser.
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
     *   if (!possibleError.checkForExpressionError())
     *       return false; // we reach this point with a pending exception
     *
     *   PossibleError possibleError(*this);
     *   possibleError.setPendingExpressionErrorAt(pos, JSMSG_BAD_PROP_ID);
     *   // Returns true, no error is reported.
     *   if (!possibleError.checkForDestructuringError())
     *       return false; // not reached, no pending exception
     *
     *   PossibleError possibleError(*this);
     *   // Returns true, no error is reported.
     *   if (!possibleError.checkForExpressionError())
     *       return false; // not reached, no pending exception
     */
    class MOZ_STACK_CLASS PossibleError
    {
      private:
        enum class ErrorKind { Expression, Destructuring, DestructuringWarning };

        enum class ErrorState { None, Pending };

        struct Error {
            ErrorState state_ = ErrorState::None;

            // Error reporting fields.
            uint32_t offset_;
            unsigned errorNumber_;
        };

        GeneralParser<ParseHandler, CharT>& parser_;
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
        MOZ_MUST_USE bool checkForError(ErrorKind kind);

        // If there is a pending warning, report it and return either false or
        // true depending on the werror option, otherwise return true.
        MOZ_MUST_USE bool checkForWarning(ErrorKind kind);

        // Transfer an existing error to another instance.
        void transferErrorTo(ErrorKind kind, PossibleError* other);

      public:
        explicit PossibleError(GeneralParser<ParseHandler, CharT>& parser);

        // Return true if a pending destructuring error is present.
        bool hasPendingDestructuringError();

        // Set a pending destructuring error. Only a single error may be set
        // per instance, i.e. subsequent calls to this method are ignored and
        // won't overwrite the existing pending error.
        void setPendingDestructuringErrorAt(const TokenPos& pos, unsigned errorNumber);

        // Set a pending destructuring warning. Only a single warning may be
        // set per instance, i.e. subsequent calls to this method are ignored
        // and won't overwrite the existing pending warning.
        void setPendingDestructuringWarningAt(const TokenPos& pos, unsigned errorNumber);

        // Set a pending expression error. Only a single error may be set per
        // instance, i.e. subsequent calls to this method are ignored and won't
        // overwrite the existing pending error.
        void setPendingExpressionErrorAt(const TokenPos& pos, unsigned errorNumber);

        // If there is a pending destructuring error or warning, report it and
        // return false, otherwise return true. Clears any pending expression
        // error.
        MOZ_MUST_USE bool checkForDestructuringErrorOrWarning();

        // If there is a pending expression error, report it and return false,
        // otherwise return true. Clears any pending destructuring error or
        // warning.
        MOZ_MUST_USE bool checkForExpressionError();

        // Pass pending errors between possible error instances. This is useful
        // for extending the lifetime of a pending error beyond the scope of
        // the PossibleError where it was initially set (keeping in mind that
        // PossibleError is a MOZ_STACK_CLASS).
        void transferErrorsTo(PossibleError* other);
    };

  private:
    // DO NOT USE THE syntaxParser_ FIELD DIRECTLY.  Use the accessors defined
    // below to access this field per its actual type.
    using Base::internalSyntaxParser_;

  protected:
    SyntaxParser* getSyntaxParser() const {
        return reinterpret_cast<SyntaxParser*>(internalSyntaxParser_);
    }

    void setSyntaxParser(SyntaxParser* syntaxParser) {
        internalSyntaxParser_ = syntaxParser;
    }

  public:
    TokenStream tokenStream;

  public:
    GeneralParser(JSContext* cx, LifoAlloc& alloc, const ReadOnlyCompileOptions& options,
                  const CharT* chars, size_t length, bool foldConstants,
                  UsedNameTracker& usedNames, SyntaxParser* syntaxParser,
                  LazyScript* lazyOuterFunction);

    inline void setAwaitHandling(AwaitHandling awaitHandling);

    /*
     * Parse a top-level JS script.
     */
    Node parse();

    /* Report the given error at the current offset. */
    void error(unsigned errorNumber, ...);
    void errorWithNotes(UniquePtr<JSErrorNotes> notes, unsigned errorNumber, ...);

    /* Report the given error at the given offset. */
    void errorAt(uint32_t offset, unsigned errorNumber, ...);
    void errorWithNotesAt(UniquePtr<JSErrorNotes> notes, uint32_t offset,
                          unsigned errorNumber, ...);

    /*
     * Handle a strict mode error at the current offset.  Report an error if in
     * strict mode code, or warn if not, using the given error number and
     * arguments.
     */
    MOZ_MUST_USE bool strictModeError(unsigned errorNumber, ...);

    /*
     * Handle a strict mode error at the given offset.  Report an error if in
     * strict mode code, or warn if not, using the given error number and
     * arguments.
     */
    MOZ_MUST_USE bool strictModeErrorAt(uint32_t offset, unsigned errorNumber, ...);

    /* Report the given warning at the current offset. */
    MOZ_MUST_USE bool warning(unsigned errorNumber, ...);

    /* Report the given warning at the given offset. */
    MOZ_MUST_USE bool warningAt(uint32_t offset, unsigned errorNumber, ...);

    bool warnOnceAboutExprClosure();

    /*
     * If extra warnings are enabled, report the given warning at the current
     * offset.
     */
    MOZ_MUST_USE bool extraWarning(unsigned errorNumber, ...);

    /*
     * If extra warnings are enabled, report the given warning at the given
     * offset.
     */
    MOZ_MUST_USE bool extraWarningAt(uint32_t offset, unsigned errorNumber, ...);

  private:
    GeneralParser* thisForCtor() { return this; }

    Node noSubstitutionUntaggedTemplate();
    Node templateLiteral(YieldHandling yieldHandling);
    bool taggedTemplate(YieldHandling yieldHandling, Node nodeList, TokenKind tt);
    bool appendToCallSiteObj(Node callSiteObj);
    bool addExprAndGetNextTemplStrToken(YieldHandling yieldHandling, Node nodeList,
                                        TokenKind* ttp);

    inline bool trySyntaxParseInnerFunction(Node* funcNode, HandleFunction fun,
                                            uint32_t toStringStart, InHandling inHandling,
                                            YieldHandling yieldHandling, FunctionSyntaxKind kind,
                                            GeneratorKind generatorKind,
                                            FunctionAsyncKind asyncKind, bool tryAnnexB,
                                            Directives inheritedDirectives,
                                            Directives* newDirectives);

    inline bool skipLazyInnerFunction(Node funcNode, uint32_t toStringStart,
                                      FunctionSyntaxKind kind, bool tryAnnexB);

  public:
    /* Public entry points for parsing. */
    Node statementListItem(YieldHandling yieldHandling, bool canHaveDirectives = false);

    // Parse an inner function given an enclosing ParseContext and a
    // FunctionBox for the inner function.
    MOZ_MUST_USE Node
    innerFunctionForFunctionBox(Node funcNode, ParseContext* outerpc, FunctionBox* funbox,
                                InHandling inHandling, YieldHandling yieldHandling,
                                FunctionSyntaxKind kind, Directives* newDirectives);

    // Parse a function's formal parameters and its body assuming its function
    // ParseContext is already on the stack.
    bool functionFormalParametersAndBody(InHandling inHandling, YieldHandling yieldHandling,
                                         Node* pn, FunctionSyntaxKind kind,
                                         const mozilla::Maybe<uint32_t>& parameterListEnd = mozilla::Nothing(),
                                         bool isStandaloneFunction = false);

  private:
    /*
     * JS parsers, from lowest to highest precedence.
     *
     * Each parser must be called during the dynamic scope of a ParseContext
     * object, pointed to by this->pc.
     *
     * Each returns a parse node tree or null on error.
     */
    Node functionStmt(uint32_t toStringStart,
                      YieldHandling yieldHandling, DefaultHandling defaultHandling,
                      FunctionAsyncKind asyncKind = FunctionAsyncKind::SyncFunction);
    Node functionExpr(uint32_t toStringStart, ExpressionClosure expressionClosureHandling,
                      InvokedPrediction invoked, FunctionAsyncKind asyncKind);

    Node statement(YieldHandling yieldHandling);
    bool maybeParseDirective(Node list, Node pn, bool* cont);

    Node blockStatement(YieldHandling yieldHandling,
                        unsigned errorNumber = JSMSG_CURLY_IN_COMPOUND);
    Node doWhileStatement(YieldHandling yieldHandling);
    Node whileStatement(YieldHandling yieldHandling);

    Node forStatement(YieldHandling yieldHandling);
    bool forHeadStart(YieldHandling yieldHandling,
                      ParseNodeKind* forHeadKind,
                      Node* forInitialPart,
                      mozilla::Maybe<ParseContext::Scope>& forLetImpliedScope,
                      Node* forInOrOfExpression);
    Node expressionAfterForInOrOf(ParseNodeKind forHeadKind, YieldHandling yieldHandling);

    Node switchStatement(YieldHandling yieldHandling);
    Node continueStatement(YieldHandling yieldHandling);
    Node breakStatement(YieldHandling yieldHandling);
    Node returnStatement(YieldHandling yieldHandling);
    Node withStatement(YieldHandling yieldHandling);
    Node throwStatement(YieldHandling yieldHandling);
    Node tryStatement(YieldHandling yieldHandling);
    Node catchBlockStatement(YieldHandling yieldHandling, ParseContext::Scope& catchParamScope);
    Node debuggerStatement();

    Node variableStatement(YieldHandling yieldHandling);

    Node labeledStatement(YieldHandling yieldHandling);
    Node labeledItem(YieldHandling yieldHandling);

    Node ifStatement(YieldHandling yieldHandling);
    Node consequentOrAlternative(YieldHandling yieldHandling);

    Node lexicalDeclaration(YieldHandling yieldHandling, DeclarationKind kind);

    inline Node importDeclaration();

    Node exportFrom(uint32_t begin, Node specList);
    Node exportBatch(uint32_t begin);
    inline bool checkLocalExportNames(Node node);
    Node exportClause(uint32_t begin);
    Node exportFunctionDeclaration(uint32_t begin, uint32_t toStringStart,
                                   FunctionAsyncKind asyncKind = FunctionAsyncKind::SyncFunction);
    Node exportVariableStatement(uint32_t begin);
    Node exportClassDeclaration(uint32_t begin);
    Node exportLexicalDeclaration(uint32_t begin, DeclarationKind kind);
    Node exportDefaultFunctionDeclaration(uint32_t begin, uint32_t toStringStart,
                                          FunctionAsyncKind asyncKind = FunctionAsyncKind::SyncFunction);
    Node exportDefaultClassDeclaration(uint32_t begin);
    Node exportDefaultAssignExpr(uint32_t begin);
    Node exportDefault(uint32_t begin);
    Node exportDeclaration();

    Node expressionStatement(YieldHandling yieldHandling,
                             InvokedPrediction invoked = PredictUninvoked);

    // Declaration parsing.  The main entrypoint is Parser::declarationList,
    // with sub-functionality split out into the remaining methods.

    // |blockScope| may be non-null only when |kind| corresponds to a lexical
    // declaration (that is, PNK_LET or PNK_CONST).
    //
    // The for* parameters, for normal declarations, should be null/ignored.
    // They should be non-null only when Parser::forHeadStart parses a
    // declaration at the start of a for-loop head.
    //
    // In this case, on success |*forHeadKind| is PNK_FORHEAD, PNK_FORIN, or
    // PNK_FOROF, corresponding to the three for-loop kinds.  The precise value
    // indicates what was parsed.
    //
    // If parsing recognized a for(;;) loop, the next token is the ';' within
    // the loop-head that separates the init/test parts.
    //
    // Otherwise, for for-in/of loops, the next token is the ')' ending the
    // loop-head.  Additionally, the expression that the loop iterates over was
    // parsed into |*forInOrOfExpression|.
    Node declarationList(YieldHandling yieldHandling,
                         ParseNodeKind kind,
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
    Node declarationPattern(DeclarationKind declKind, TokenKind tt,
                            bool initialDeclaration, YieldHandling yieldHandling,
                            ParseNodeKind* forHeadKind, Node* forInOrOfExpression);
    Node declarationName(DeclarationKind declKind, TokenKind tt,
                         bool initialDeclaration, YieldHandling yieldHandling,
                         ParseNodeKind* forHeadKind, Node* forInOrOfExpression);

    // Having parsed a name (not found in a destructuring pattern) declared by
    // a declaration, with the current token being the '=' separating the name
    // from its initializer, parse and bind that initializer -- and possibly
    // consume trailing in/of and subsequent expression, if so directed by
    // |forHeadKind|.
    bool initializerInNameDeclaration(Node binding,
                                      DeclarationKind declKind, bool initialDeclaration,
                                      YieldHandling yieldHandling, ParseNodeKind* forHeadKind,
                                      Node* forInOrOfExpression);

    Node expr(InHandling inHandling, YieldHandling yieldHandling,
              TripledotHandling tripledotHandling, PossibleError* possibleError = nullptr,
              InvokedPrediction invoked = PredictUninvoked);
    Node assignExpr(InHandling inHandling, YieldHandling yieldHandling,
                    TripledotHandling tripledotHandling, PossibleError* possibleError = nullptr,
                    InvokedPrediction invoked = PredictUninvoked);
    Node assignExprWithoutYieldOrAwait(YieldHandling yieldHandling);
    Node yieldExpression(InHandling inHandling);
    Node condExpr(InHandling inHandling, YieldHandling yieldHandling,
                  TripledotHandling tripledotHandling, ExpressionClosure expressionClosureHandling,
                  PossibleError* possibleError,
                  InvokedPrediction invoked = PredictUninvoked);
    Node orExpr(InHandling inHandling, YieldHandling yieldHandling,
                TripledotHandling tripledotHandling, ExpressionClosure expressionClosureHandling,
                PossibleError* possibleError,
                InvokedPrediction invoked = PredictUninvoked);
    Node unaryExpr(YieldHandling yieldHandling, TripledotHandling tripledotHandling,
                   ExpressionClosure expressionClosureHandling,
                   PossibleError* possibleError = nullptr,
                   InvokedPrediction invoked = PredictUninvoked);
    Node memberExpr(YieldHandling yieldHandling, TripledotHandling tripledotHandling,
                    ExpressionClosure expressionClosureHandling, TokenKind tt,
                    bool allowCallSyntax = true, PossibleError* possibleError = nullptr,
                    InvokedPrediction invoked = PredictUninvoked);
    Node primaryExpr(YieldHandling yieldHandling, TripledotHandling tripledotHandling,
                     ExpressionClosure expressionClosureHandling, TokenKind tt,
                     PossibleError* possibleError, InvokedPrediction invoked = PredictUninvoked);
    Node exprInParens(InHandling inHandling, YieldHandling yieldHandling,
                      TripledotHandling tripledotHandling, PossibleError* possibleError = nullptr);

    bool tryNewTarget(Node& newTarget);

    Node methodDefinition(uint32_t toStringStart, PropertyType propType, HandleAtom funName);

    /*
     * Additional JS parsers.
     */
    bool functionArguments(YieldHandling yieldHandling, FunctionSyntaxKind kind,
                           Node funcpn);

    Node functionDefinition(Node funcNode, uint32_t toStringStart, InHandling inHandling,
                            YieldHandling yieldHandling, HandleAtom name, FunctionSyntaxKind kind,
                            GeneratorKind generatorKind, FunctionAsyncKind asyncKind,
                            bool tryAnnexB = false);

    // Parse a function body.  Pass StatementListBody if the body is a list of
    // statements; pass ExpressionBody if the body is a single expression.
    enum FunctionBodyType { StatementListBody, ExpressionBody };
    Node functionBody(InHandling inHandling, YieldHandling yieldHandling, FunctionSyntaxKind kind,
                      FunctionBodyType type);

    Node unaryOpExpr(YieldHandling yieldHandling, ParseNodeKind kind, uint32_t begin);

    Node condition(InHandling inHandling, YieldHandling yieldHandling);

    bool argumentList(YieldHandling yieldHandling, Node listNode, bool* isSpread,
                      PossibleError* possibleError = nullptr);
    Node destructuringDeclaration(DeclarationKind kind, YieldHandling yieldHandling,
                                  TokenKind tt);
    Node destructuringDeclarationWithoutYieldOrAwait(DeclarationKind kind, YieldHandling yieldHandling,
                                                     TokenKind tt);

    inline bool checkExportedName(JSAtom* exportName);
    inline bool checkExportedNamesForArrayBinding(Node node);
    inline bool checkExportedNamesForObjectBinding(Node node);
    inline bool checkExportedNamesForDeclaration(Node node);
    inline bool checkExportedNamesForDeclarationList(Node node);
    inline bool checkExportedNameForFunction(Node node);
    inline bool checkExportedNameForClass(Node node);
    inline bool checkExportedNameForClause(Node node);

    enum ClassContext { ClassStatement, ClassExpression };
    Node classDefinition(YieldHandling yieldHandling, ClassContext classContext,
                         DefaultHandling defaultHandling);

    bool checkBindingIdentifier(PropertyName* ident,
                                uint32_t offset,
                                YieldHandling yieldHandling,
                                TokenKind hint = TokenKind::Limit);

    PropertyName* labelOrIdentifierReference(YieldHandling yieldHandling);

    PropertyName* labelIdentifier(YieldHandling yieldHandling) {
        return labelOrIdentifierReference(yieldHandling);
    }

    PropertyName* identifierReference(YieldHandling yieldHandling) {
        return labelOrIdentifierReference(yieldHandling);
    }

    bool matchLabel(YieldHandling yieldHandling, MutableHandle<PropertyName*> label);

    // Indicate if the next token (tokenized as Operand) is |in| or |of|.  If
    // so, consume it.
    bool matchInOrOf(bool* isForInp, bool* isForOfp);

  private:
    bool checkIncDecOperand(Node operand, uint32_t operandOffset);
    bool checkStrictAssignment(Node lhs);

    void reportMissingClosing(unsigned errorNumber, unsigned noteNumber, uint32_t openedPos);

    void reportRedeclaration(HandlePropertyName name, DeclarationKind prevKind, TokenPos pos,
                             uint32_t prevPos);
    bool notePositionalFormalParameter(Node fn, HandlePropertyName name, uint32_t beginPos,
                                       bool disallowDuplicateParams, bool* duplicatedParam);

    bool checkLexicalDeclarationDirectlyWithinBlock(ParseContext::Statement& stmt,
                                                    DeclarationKind kind, TokenPos pos);

    Node propertyName(YieldHandling yieldHandling,
                      const mozilla::Maybe<DeclarationKind>& maybeDecl, Node propList,
                      PropertyType* propType, MutableHandleAtom propAtom);
    Node computedPropertyName(YieldHandling yieldHandling,
                              const mozilla::Maybe<DeclarationKind>& maybeDecl, Node literal);
    Node arrayInitializer(YieldHandling yieldHandling, PossibleError* possibleError);
    inline Node newRegExp();

    Node objectLiteral(YieldHandling yieldHandling, PossibleError* possibleError);

    Node bindingInitializer(Node lhs, DeclarationKind kind, YieldHandling yieldHandling);
    Node bindingIdentifier(DeclarationKind kind, YieldHandling yieldHandling);
    Node bindingIdentifierOrPattern(DeclarationKind kind, YieldHandling yieldHandling,
                                    TokenKind tt);
    Node objectBindingPattern(DeclarationKind kind, YieldHandling yieldHandling);
    Node arrayBindingPattern(DeclarationKind kind, YieldHandling yieldHandling);

    enum class TargetBehavior {
        PermitAssignmentPattern,
        ForbidAssignmentPattern
    };
    bool checkDestructuringAssignmentTarget(Node expr, TokenPos exprPos,
                                            PossibleError* exprPossibleError,
                                            PossibleError* possibleError,
                                            TargetBehavior behavior = TargetBehavior::PermitAssignmentPattern);
    void checkDestructuringAssignmentName(Node name, TokenPos namePos,
                                          PossibleError* possibleError);
    bool checkDestructuringAssignmentElement(Node expr, TokenPos exprPos,
                                             PossibleError* exprPossibleError,
                                             PossibleError* possibleError);

    Node newNumber(const Token& tok) {
        return handler.newNumber(tok.number(), tok.decimalPoint(), tok.pos);
    }

  protected:
    // Match the current token against the BindingIdentifier production with
    // the given Yield parameter.  If there is no match, report a syntax
    // error.
    PropertyName* bindingIdentifier(YieldHandling yieldHandling);

    bool checkLabelOrIdentifierReference(PropertyName* ident, uint32_t offset,
                                         YieldHandling yieldHandling,
                                         TokenKind hint = TokenKind::Limit);

    Node statementList(YieldHandling yieldHandling);

    MOZ_MUST_USE Node
    innerFunction(Node funcNode, ParseContext* outerpc, HandleFunction fun,
                  uint32_t toStringStart, InHandling inHandling, YieldHandling yieldHandling,
                  FunctionSyntaxKind kind, GeneratorKind generatorKind,
                  FunctionAsyncKind asyncKind, bool tryAnnexB, Directives inheritedDirectives,
                  Directives* newDirectives);

    bool matchOrInsertSemicolon();

    bool noteDeclaredName(HandlePropertyName name, DeclarationKind kind, TokenPos pos);

  private:
    inline bool asmJS(Node list);
};

template <typename CharT>
class Parser<SyntaxParseHandler, CharT> final
  : public GeneralParser<SyntaxParseHandler, CharT>
{
    using Base = GeneralParser<SyntaxParseHandler, CharT>;
    using Node = SyntaxParseHandler::Node;

    using SyntaxParser = Parser<SyntaxParseHandler, CharT>;

    // Numerous Base::* functions have bodies like
    //
    //   return asFinalParser()->func(...);
    //
    // and must be able to call functions here.  Add a friendship relationship
    // so functions here can be hidden when appropriate.
    friend class GeneralParser<SyntaxParseHandler, CharT>;

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
    using Base::context;
    using Base::hadAbortedSyntaxParse;
    using Base::innerFunctionForFunctionBox;
    using Base::tokenStream;

  private:
    using Base::alloc;
#if DEBUG
    using Base::checkOptionsCalled;
#endif
    using Base::error;
    using Base::errorAt;
    using Base::finishFunctionScopes;
    using Base::functionFormalParametersAndBody;
    using Base::handler;
    using Base::innerFunction;
    using Base::keepAtoms;
    using Base::matchOrInsertSemicolon;
    using Base::newFunctionBox;
    using Base::newLexicalScopeData;
    using Base::newModuleScopeData;
    using Base::newName;
    using Base::noteDeclaredName;
    using Base::null;
    using Base::options;
    using Base::pc;
    using Base::pos;
    using Base::propagateFreeNamesAndMarkClosedOverBindings;
    using Base::ss;
    using Base::statementList;
    using Base::stringLiteral;
    using Base::usedNames;

  private:
    using Base::abortIfSyntaxParser;
    using Base::disableSyntaxParser;

  public:
    // Functions with multiple overloads of different visibility.  We can't
    // |using| the whole thing into existence because of the visibility
    // distinction, so we instead must manually delegate the required overload.

    PropertyName* bindingIdentifier(YieldHandling yieldHandling) {
        return Base::bindingIdentifier(yieldHandling);
    }

    // Functions present in both Parser<ParseHandler, CharT> specializations.

    inline void setAwaitHandling(AwaitHandling awaitHandling);

    Node newRegExp();

    // Parse a module.
    Node moduleBody(ModuleSharedContext* modulesc);

    inline Node importDeclaration();
    inline bool checkLocalExportNames(Node node);
    inline bool checkExportedName(JSAtom* exportName);
    inline bool checkExportedNamesForArrayBinding(Node node);
    inline bool checkExportedNamesForObjectBinding(Node node);
    inline bool checkExportedNamesForDeclaration(Node node);
    inline bool checkExportedNamesForDeclarationList(Node node);
    inline bool checkExportedNameForFunction(Node node);
    inline bool checkExportedNameForClass(Node node);
    inline bool checkExportedNameForClause(Node node);

    bool trySyntaxParseInnerFunction(Node* funcNode, HandleFunction fun, uint32_t toStringStart,
                                     InHandling inHandling, YieldHandling yieldHandling,
                                     FunctionSyntaxKind kind, GeneratorKind generatorKind,
                                     FunctionAsyncKind asyncKind, bool tryAnnexB,
                                     Directives inheritedDirectives, Directives* newDirectives);

    bool skipLazyInnerFunction(Node funcNode, uint32_t toStringStart, FunctionSyntaxKind kind,
                               bool tryAnnexB);

    bool asmJS(Node list);

    // Functions present only in Parser<SyntaxParseHandler, CharT>.
};

template <typename CharT>
class Parser<FullParseHandler, CharT> final
  : public GeneralParser<FullParseHandler, CharT>
{
    using Base = GeneralParser<FullParseHandler, CharT>;
    using Node = FullParseHandler::Node;

    using SyntaxParser = Parser<SyntaxParseHandler, CharT>;

    // Numerous Base::* functions have bodies like
    //
    //   return asFinalParser()->func(...);
    //
    // and must be able to call functions here.  Add a friendship relationship
    // so functions here can be hidden when appropriate.
    friend class GeneralParser<FullParseHandler, CharT>;

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
    using Base::handler;
    using Base::newFunctionBox;
    using Base::options;
    using Base::pc;
    using Base::pos;
    using Base::ss;
    using Base::tokenStream;
    using Base::allowExpressionClosures;

  private:
    using Base::alloc;
    using Base::checkLabelOrIdentifierReference;
#if DEBUG
    using Base::checkOptionsCalled;
#endif
    using Base::context;
    using Base::error;
    using Base::errorAt;
    using Base::finishFunctionScopes;
    using Base::finishLexicalScope;
    using Base::innerFunction;
    using Base::innerFunctionForFunctionBox;
    using Base::keepAtoms;
    using Base::matchOrInsertSemicolon;
    using Base::newEvalScopeData;
    using Base::newFunctionScopeData;
    using Base::newGlobalScopeData;
    using Base::newLexicalScopeData;
    using Base::newModuleScopeData;
    using Base::newName;
    using Base::newVarScopeData;
    using Base::noteDeclaredName;
    using Base::null;
    using Base::propagateFreeNamesAndMarkClosedOverBindings;
    using Base::statementList;
    using Base::stringLiteral;
    using Base::usedNames;

    using Base::abortIfSyntaxParser;
    using Base::disableSyntaxParser;
    using Base::getSyntaxParser;
    using Base::setSyntaxParser;

  public:
    // Functions with multiple overloads of different visibility.  We can't
    // |using| the whole thing into existence because of the visibility
    // distinction, so we instead must manually delegate the required overload.

    PropertyName* bindingIdentifier(YieldHandling yieldHandling) {
        return Base::bindingIdentifier(yieldHandling);
    }

    // Functions present in both Parser<ParseHandler, CharT> specializations.

    friend class AutoAwaitIsKeyword<SyntaxParseHandler, CharT>;
    inline void setAwaitHandling(AwaitHandling awaitHandling);

    Node newRegExp();

    // Parse a module.
    Node moduleBody(ModuleSharedContext* modulesc);

    Node importDeclaration();
    bool checkLocalExportNames(Node node);
    bool checkExportedName(JSAtom* exportName);
    bool checkExportedNamesForArrayBinding(Node node);
    bool checkExportedNamesForObjectBinding(Node node);
    bool checkExportedNamesForDeclaration(Node node);
    bool checkExportedNamesForDeclarationList(Node node);
    bool checkExportedNameForFunction(Node node);
    bool checkExportedNameForClass(Node node);
    inline bool checkExportedNameForClause(Node node);

    bool trySyntaxParseInnerFunction(Node* funcNode, HandleFunction fun, uint32_t toStringStart,
                                     InHandling inHandling, YieldHandling yieldHandling,
                                     FunctionSyntaxKind kind, GeneratorKind generatorKind,
                                     FunctionAsyncKind asyncKind, bool tryAnnexB,
                                     Directives inheritedDirectives, Directives* newDirectives);

    bool skipLazyInnerFunction(Node funcNode, uint32_t toStringStart, FunctionSyntaxKind kind,
                               bool tryAnnexB);

    // Functions present only in Parser<FullParseHandler, CharT>.

    // Parse the body of an eval.
    //
    // Eval scripts are distinguished from global scripts in that in ES6, per
    // 18.2.1.1 steps 9 and 10, all eval scripts are executed under a fresh
    // lexical scope.
    Node evalBody(EvalSharedContext* evalsc);

    // Parse a function, given only its arguments and body. Used for lazily
    // parsed functions.
    Node standaloneLazyFunction(HandleFunction fun, uint32_t toStringStart, bool strict,
                                GeneratorKind generatorKind, FunctionAsyncKind asyncKind);

    // Parse a function, used for the Function, GeneratorFunction, and
    // AsyncFunction constructors.
    Node standaloneFunction(HandleFunction fun, HandleScope enclosingScope,
                            const mozilla::Maybe<uint32_t>& parameterListEnd,
                            GeneratorKind generatorKind, FunctionAsyncKind asyncKind,
                            Directives inheritedDirectives, Directives* newDirectives);

    bool checkStatementsEOF();

    // Parse the body of a global script.
    Node globalBody(GlobalSharedContext* globalsc);

    bool namedImportsOrNamespaceImport(TokenKind tt, Node importSpecSet);

    PropertyName* importedBinding() {
        return bindingIdentifier(YieldIsName);
    }

    bool checkLocalExportName(PropertyName* ident, uint32_t offset) {
        return checkLabelOrIdentifierReference(ident, offset, YieldIsName);
    }

    bool asmJS(Node list);
};

template<class Parser>
/* static */ inline const TokenStreamAnyChars&
ParserAnyCharsAccess<Parser>::anyChars(const GeneralTokenStreamChars* ts)
{
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

    static_assert(mozilla::IsBaseOf<GeneralTokenStreamChars,
                                    TokenStreamSpecific>::value,
                  "the static_cast<> below assumes a base-class relationship");
    const auto* tss = static_cast<const TokenStreamSpecific*>(ts);

    auto tssAddr = reinterpret_cast<uintptr_t>(tss);

    using ActualTokenStreamType = decltype(static_cast<Parser*>(nullptr)->tokenStream);
    static_assert(mozilla::IsSame<ActualTokenStreamType, TokenStreamSpecific>::value,
                                  "Parser::tokenStream must have type TokenStreamSpecific");

    uintptr_t parserAddr = tssAddr - offsetof(Parser, tokenStream);

    return reinterpret_cast<const Parser*>(parserAddr)->anyChars;
}

template<class Parser>
/* static */ inline TokenStreamAnyChars&
ParserAnyCharsAccess<Parser>::anyChars(GeneralTokenStreamChars* ts)
{
    const TokenStreamAnyChars& anyCharsConst =
        anyChars(const_cast<const GeneralTokenStreamChars*>(ts));

    return const_cast<TokenStreamAnyChars&>(anyCharsConst);
}

template <class ParseHandler, typename CharT>
class MOZ_STACK_CLASS AutoAwaitIsKeyword
{
    using GeneralParser = frontend::GeneralParser<ParseHandler, CharT>;

  private:
    GeneralParser* parser_;
    AwaitHandling oldAwaitHandling_;

  public:
    AutoAwaitIsKeyword(GeneralParser* parser, AwaitHandling awaitHandling) {
        parser_ = parser;
        oldAwaitHandling_ = static_cast<AwaitHandling>(parser_->awaitHandling_);

        // 'await' is always a keyword in module contexts, so we don't modify
        // the state when the original handling is AwaitIsModuleKeyword.
        if (oldAwaitHandling_ != AwaitIsModuleKeyword)
            parser_->setAwaitHandling(awaitHandling);
    }

    ~AutoAwaitIsKeyword() {
        parser_->setAwaitHandling(oldAwaitHandling_);
    }
};

template <typename Scope>
extern typename Scope::Data*
NewEmptyBindingData(JSContext* cx, LifoAlloc& alloc, uint32_t numBindings);

Maybe<GlobalScope::Data*>
NewGlobalScopeData(JSContext* context, ParseContext::Scope& scope, LifoAlloc& alloc, ParseContext* pc);
Maybe<EvalScope::Data*>
NewEvalScopeData(JSContext* context, ParseContext::Scope& scope, LifoAlloc& alloc, ParseContext* pc);
Maybe<FunctionScope::Data*>
NewFunctionScopeData(JSContext* context, ParseContext::Scope& scope, bool hasParameterExprs, LifoAlloc& alloc, ParseContext* pc);
Maybe<VarScope::Data*>
NewVarScopeData(JSContext* context, ParseContext::Scope& scope, LifoAlloc& alloc, ParseContext* pc);
Maybe<LexicalScope::Data*>
NewLexicalScopeData(JSContext* context, ParseContext::Scope& scope, LifoAlloc& alloc, ParseContext* pc);

} /* namespace frontend */
} /* namespace js */

#endif /* frontend_Parser_h */
