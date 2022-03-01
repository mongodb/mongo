/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * A variant-like class abstracting operations on a Parser with a given
 * ParseHandler but unspecified character type.
 */

#ifndef frontend_EitherParser_h
#define frontend_EitherParser_h

#include "mozilla/Tuple.h"
#include "mozilla/Utf8.h"
#include "mozilla/Variant.h"

#include <type_traits>
#include <utility>

#include "frontend/BCEParserHandle.h"
#include "frontend/Parser.h"
#include "frontend/TokenStream.h"

namespace js {

namespace detail {

template <template <class Parser> class GetThis,
          template <class This> class MemberFunction, typename... Args>
struct InvokeMemberFunction {
  mozilla::Tuple<std::decay_t<Args>...> args;

  template <class This, size_t... Indices>
  auto matchInternal(This* obj, std::index_sequence<Indices...>) -> decltype((
      (*obj).*(MemberFunction<This>::get()))(mozilla::Get<Indices>(args)...)) {
    return ((*obj).*
            (MemberFunction<This>::get()))(mozilla::Get<Indices>(args)...);
  }

 public:
  template <typename... ActualArgs>
  explicit InvokeMemberFunction(ActualArgs&&... actualArgs)
      : args{std::forward<ActualArgs>(actualArgs)...} {}

  template <class Parser>
  auto operator()(Parser* parser)
      -> decltype(this->matchInternal(GetThis<Parser>::get(parser),
                                      std::index_sequence_for<Args...>{})) {
    return this->matchInternal(GetThis<Parser>::get(parser),
                               std::index_sequence_for<Args...>{});
  }
};

// |this|-computing templates.

template <class Parser>
struct GetParser {
  static Parser* get(Parser* parser) { return parser; }
};

template <class Parser>
struct GetTokenStream {
  static auto get(Parser* parser) { return &parser->tokenStream; }
};

// Member function-computing templates.

template <class Parser>
struct ParserOptions {
  static constexpr auto get() { return &Parser::options; }
};

template <class TokenStream>
struct TokenStreamComputeLineAndColumn {
  static constexpr auto get() { return &TokenStream::computeLineAndColumn; }
};

// Generic matchers.

struct ParseHandlerMatcher {
  template <class Parser>
  frontend::FullParseHandler& operator()(Parser* parser) {
    return parser->handler_;
  }
};

struct ParserSharedBaseMatcher {
  template <class Parser>
  frontend::ParserSharedBase& operator()(Parser* parser) {
    return *static_cast<frontend::ParserSharedBase*>(parser);
  }
};

struct ErrorReporterMatcher {
  template <class Parser>
  frontend::ErrorReporter& operator()(Parser* parser) {
    return parser->tokenStream;
  }
};

}  // namespace detail

namespace frontend {

class EitherParser : public BCEParserHandle {
  // Leave this as a variant, to promote good form until 8-bit parser
  // integration.
  mozilla::Variant<Parser<FullParseHandler, char16_t>* const,
                   Parser<FullParseHandler, mozilla::Utf8Unit>* const>
      parser;

  using Node = typename FullParseHandler::Node;

  template <template <class Parser> class GetThis,
            template <class This> class GetMemberFunction,
            typename... StoredArgs>
  using InvokeMemberFunction =
      detail::InvokeMemberFunction<GetThis, GetMemberFunction, StoredArgs...>;

 public:
  template <class Parser>
  explicit EitherParser(Parser* parser) : parser(parser) {}

  FullParseHandler& astGenerator() final {
    return parser.match(detail::ParseHandlerMatcher());
  }

  ErrorReporter& errorReporter() final {
    return parser.match(detail::ErrorReporterMatcher());
  }
  const ErrorReporter& errorReporter() const final {
    return parser.match(detail::ErrorReporterMatcher());
  }

  const JS::ReadOnlyCompileOptions& options() const final {
    InvokeMemberFunction<detail::GetParser, detail::ParserOptions>
        optionsMatcher;
    return parser.match(std::move(optionsMatcher));
  }

  void computeLineAndColumn(uint32_t offset, uint32_t* line,
                            uint32_t* column) const {
    InvokeMemberFunction<detail::GetTokenStream,
                         detail::TokenStreamComputeLineAndColumn, uint32_t,
                         uint32_t*, uint32_t*>
        matcher{offset, line, column};
    return parser.match(std::move(matcher));
  }

  CompilationState& getCompilationState() {
    ParserSharedBase& base = parser.match(detail::ParserSharedBaseMatcher());
    return base.getCompilationState();
  }

  ParserAtomsTable& parserAtoms() {
    ParserSharedBase& base = parser.match(detail::ParserSharedBaseMatcher());
    return base.parserAtoms();
  }
};

} /* namespace frontend */
} /* namespace js */

#endif /* frontend_EitherParser_h */
