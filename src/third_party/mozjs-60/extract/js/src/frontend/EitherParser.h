/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * A variant-like class abstracting operations on a Parser with a given ParseHandler but
 * unspecified character type.
 */

#ifndef frontend_EitherParser_h
#define frontend_EitherParser_h

#include "mozilla/Attributes.h"
#include "mozilla/IndexSequence.h"
#include "mozilla/Move.h"
#include "mozilla/Tuple.h"
#include "mozilla/TypeTraits.h"
#include "mozilla/Variant.h"

#include "frontend/Parser.h"
#include "frontend/TokenStream.h"

namespace js {

namespace detail {

template<template <class Parser> class GetThis,
         template <class This> class MemberFunction,
         typename... Args>
struct InvokeMemberFunction
{
    mozilla::Tuple<typename mozilla::Decay<Args>::Type...> args;

    template<class This, size_t... Indices>
    auto
    matchInternal(This* obj, mozilla::IndexSequence<Indices...>)
      -> decltype(((*obj).*(MemberFunction<This>::get()))(mozilla::Get<Indices>(args)...))
    {
        return ((*obj).*(MemberFunction<This>::get()))(mozilla::Get<Indices>(args)...);
    }

  public:
    template<typename... ActualArgs>
    explicit InvokeMemberFunction(ActualArgs&&... actualArgs)
      : args { mozilla::Forward<ActualArgs>(actualArgs)... }
    {}

    template<class Parser>
    auto
    match(Parser* parser)
      -> decltype(this->matchInternal(GetThis<Parser>::get(parser),
                                      typename mozilla::IndexSequenceFor<Args...>::Type()))
    {
        return this->matchInternal(GetThis<Parser>::get(parser),
                                   typename mozilla::IndexSequenceFor<Args...>::Type());
    }
};

// |this|-computing templates.

template<class Parser>
struct GetParser
{
    static Parser* get(Parser* parser) { return parser; }
};

template<class Parser>
struct GetParseHandler
{
    static auto get(Parser* parser) -> decltype(&parser->handler) {
        return &parser->handler;
    }
};

template<class Parser>
struct GetTokenStream
{
    static auto get(Parser* parser) -> decltype(&parser->tokenStream) {
        return &parser->tokenStream;
    }
};

// Member function-computing templates.

template<class Parser>
struct ParserOptions
{
    static constexpr auto get() -> decltype(&Parser::options) {
        return &Parser::options;
    }
};

template<class TokenStream>
struct TokenStreamComputeErrorMetadata
{
    static constexpr auto get() -> decltype(&TokenStream::computeErrorMetadata) {
        return &TokenStream::computeErrorMetadata;
    }
};

template<class Parser>
struct ParserNewObjectBox
{
    static constexpr auto get() -> decltype(&Parser::newObjectBox) {
        return &Parser::newObjectBox;
    }
};

template<class Handler>
struct HandlerSingleBindingFromDeclaration
{
    static constexpr auto get() -> decltype(&Handler::singleBindingFromDeclaration) {
        return &Handler::singleBindingFromDeclaration;
    }
};

template<class Handler>
struct HandlerIsDeclarationList
{
    static constexpr auto get() -> decltype(&Handler::isDeclarationList) {
        return &Handler::isDeclarationList;
    }
};

template<class Handler>
struct HandlerIsSuperBase
{
    static constexpr auto get() -> decltype(&Handler::isSuperBase) {
        return &Handler::isSuperBase;
    }
};

template<class TokenStream>
struct TokenStreamReportError
{
    static constexpr auto get() -> decltype(&TokenStream::reportError) {
        return &TokenStream::reportError;
    }
};

template<class TokenStream>
struct TokenStreamReportExtraWarning
{
    static constexpr auto get() -> decltype(&TokenStream::reportExtraWarningErrorNumberVA) {
        return &TokenStream::reportExtraWarningErrorNumberVA;
    }
};

// Generic matchers.

struct TokenStreamMatcher
{
    template<class Parser>
    frontend::TokenStreamAnyChars& match(Parser* parser) {
        return parser->anyChars;
    }
};

struct ScriptSourceMatcher
{
    template<class Parser>
    ScriptSource* match(Parser* parser) {
        return parser->ss;
    }
};

struct ParserBaseMatcher
{
    template<class Parser>
    frontend::ParserBase& match(Parser* parser) {
        return *static_cast<frontend::ParserBase*>(parser);
    }
};

} // namespace detail

namespace frontend {

template<class ParseHandler>
class EitherParser
{
    mozilla::Variant<Parser<ParseHandler, char16_t>* const> parser;

    using Node = typename ParseHandler::Node;

    template<template <class Parser> class GetThis,
             template <class This> class GetMemberFunction,
             typename... StoredArgs>
    using InvokeMemberFunction =
        detail::InvokeMemberFunction<GetThis, GetMemberFunction, StoredArgs...>;

  public:
    template<class Parser>
    explicit EitherParser(Parser* parser) : parser(parser) {}

    TokenStreamAnyChars& tokenStream() {
        return parser.match(detail::TokenStreamMatcher());
    }

    const TokenStreamAnyChars& tokenStream() const {
        return parser.match(detail::TokenStreamMatcher());
    }

    ScriptSource* ss() {
        return parser.match(detail::ScriptSourceMatcher());
    }

    const JS::ReadOnlyCompileOptions& options() {
        InvokeMemberFunction<detail::GetParser, detail::ParserOptions> optionsMatcher;
        return parser.match(mozilla::Move(optionsMatcher));
    }

    MOZ_MUST_USE bool computeErrorMetadata(ErrorMetadata* metadata, uint32_t offset) {
        InvokeMemberFunction<detail::GetTokenStream, detail::TokenStreamComputeErrorMetadata,
                             ErrorMetadata*, uint32_t>
            matcher { metadata, offset };
        return parser.match(mozilla::Move(matcher));
    }

    ObjectBox* newObjectBox(JSObject* obj) {
        InvokeMemberFunction<detail::GetParser, detail::ParserNewObjectBox,
                             JSObject*>
            matcher { obj };
        return parser.match(mozilla::Move(matcher));
    }

    Node singleBindingFromDeclaration(Node decl) {
        InvokeMemberFunction<detail::GetParseHandler, detail::HandlerSingleBindingFromDeclaration,
                             Node>
            matcher { decl };
        return parser.match(mozilla::Move(matcher));
    }

    bool isDeclarationList(Node node) {
        InvokeMemberFunction<detail::GetParseHandler, detail::HandlerIsDeclarationList,
                             Node>
            matcher { node };
        return parser.match(mozilla::Move(matcher));
    }

    bool isSuperBase(Node node) {
        InvokeMemberFunction<detail::GetParseHandler, detail::HandlerIsSuperBase,
                             Node>
            matcher { node };
        return parser.match(mozilla::Move(matcher));
    }

    template<typename... Args>
    void reportError(Args&&... args) {
        InvokeMemberFunction<detail::GetTokenStream, detail::TokenStreamReportError, Args...>
            matcher { mozilla::Forward<Args>(args)... };
        return parser.match(mozilla::Move(matcher));
    }

    template<typename... Args>
    MOZ_MUST_USE bool warningNoOffset(Args&&... args) {
        return parser.match(detail::ParserBaseMatcher()).warningNoOffset(mozilla::Forward<Args>(args)...);
    }

    template<typename... Args>
    MOZ_MUST_USE bool reportExtraWarningErrorNumberVA(Args&&... args) {
        InvokeMemberFunction<detail::GetTokenStream, detail::TokenStreamReportExtraWarning, Args...>
            matcher { mozilla::Forward<Args>(args)... };
        return parser.match(mozilla::Move(matcher));
    }
};

} /* namespace frontend */
} /* namespace js */

#endif /* frontend_EitherParser_h */
