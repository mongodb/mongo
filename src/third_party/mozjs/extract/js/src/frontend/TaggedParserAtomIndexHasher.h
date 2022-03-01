/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_TaggedParserAtomIndexHasher_h
#define frontend_TaggedParserAtomIndexHasher_h

#include "frontend/ParserAtom.h"  // TaggedParserAtomIndex, TrivialTaggedParserAtomIndex
#include "js/HashTable.h"  // HashNumber

namespace js {
namespace frontend {

class TaggedParserAtomIndexHasher {
 public:
  using Lookup = TaggedParserAtomIndex;

  static inline HashNumber hash(const Lookup& l) {
    return HashNumber(l.rawData());
  }
  static inline bool match(TaggedParserAtomIndex entry, const Lookup& l) {
    return l == entry;
  }
};

class TrivialTaggedParserAtomIndexHasher {
 public:
  using Lookup = TrivialTaggedParserAtomIndex;

  static inline HashNumber hash(const Lookup& l) {
    return HashNumber(l.rawData());
  }
  static inline bool match(TrivialTaggedParserAtomIndex entry,
                           const Lookup& l) {
    return l == entry;
  }
};

}  // namespace frontend
}  // namespace js

#endif  // frontend_TaggedParserAtomIndexHasher_h
