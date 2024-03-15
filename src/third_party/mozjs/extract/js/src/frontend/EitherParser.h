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

#include "mozilla/Utf8.h"
#include "mozilla/Variant.h"

#include <type_traits>
#include <utility>

#include "frontend/Parser.h"

namespace js::frontend {

class EitherParser final {
  // Leave this as a variant, to promote good form until 8-bit parser
  // integration.
  mozilla::Variant<Parser<FullParseHandler, char16_t>* const,
                   Parser<FullParseHandler, mozilla::Utf8Unit>* const>
      parser;

 public:
  template <class Parser>
  explicit EitherParser(Parser* parser) : parser(parser) {}

  const ErrorReporter& errorReporter() const {
    return parser.match([](auto* parser) -> const frontend::ErrorReporter& {
      return parser->tokenStream;
    });
  }

  void computeLineAndColumn(uint32_t offset, uint32_t* line,
                            uint32_t* column) const {
    return parser.match([offset, line, column](auto* parser) -> void {
      parser->tokenStream.computeLineAndColumn(offset, line, column);
    });
  }

  ParserAtomsTable& parserAtoms() {
    auto& base = parser.match(
        [](auto* parser) -> frontend::ParserSharedBase& { return *parser; });
    return base.parserAtoms();
  }
};

} /* namespace js::frontend */

#endif /* frontend_EitherParser_h */
