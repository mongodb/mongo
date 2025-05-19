/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "js/Id.h"
#include "js/Printer.h"  // js::GenericPrinter, js::Fprinter
#include "js/RootingAPI.h"

#include "vm/JSContext.h"
#include "vm/JSONPrinter.h"  // js::JSONPrinter
#include "vm/SymbolType.h"

#include "vm/JSAtomUtils-inl.h"  // AtomToId

using namespace js;

static const JS::PropertyKey voidKeyValue = JS::PropertyKey::Void();

const JS::HandleId JS::VoidHandlePropertyKey =
    JS::HandleId::fromMarkedLocation(&voidKeyValue);

bool JS::PropertyKey::isPrivateName() const {
  return isSymbol() && toSymbol()->isPrivateName();
}

bool JS::PropertyKey::isWellKnownSymbol(JS::SymbolCode code) const {
  MOZ_ASSERT(uint32_t(code) < WellKnownSymbolLimit);
  if (!isSymbol()) {
    return false;
  }
  return toSymbol()->code() == code;
}

/* static */ JS::PropertyKey JS::PropertyKey::fromPinnedString(JSString* str) {
  MOZ_ASSERT(AtomIsPinned(TlsContext.get(), &str->asAtom()));
  return js::AtomToId(&str->asAtom());
}

/* static */ bool JS::PropertyKey::isNonIntAtom(JSAtom* atom) {
  uint32_t index;
  if (!atom->isIndex(&index)) {
    return true;
  }
  static_assert(PropertyKey::IntMin == 0);
  return index > PropertyKey::IntMax;
}

/* static */ bool JS::PropertyKey::isNonIntAtom(JSString* str) {
  return JS::PropertyKey::isNonIntAtom(&str->asAtom());
}

#if defined(DEBUG) || defined(JS_JITSPEW)

void JS::PropertyKey::dump() const {
  js::Fprinter out(stderr);
  dump(out);
}

void JS::PropertyKey::dump(js::GenericPrinter& out) const {
  js::JSONPrinter json(out);
  dump(json);
  out.put("\n");
}

void JS::PropertyKey::dump(js::JSONPrinter& json) const {
  json.beginObject();
  dumpFields(json);
  json.endObject();
}

void JS::PropertyKey::dumpFields(js::JSONPrinter& json) const {
  if (isAtom()) {
    json.property("type", "atom");
    toAtom()->dumpFields(json);
  } else if (isInt()) {
    json.property("type", "int");
    json.property("value", toInt());
  } else if (isSymbol()) {
    json.property("type", "symbol");
    toSymbol()->dumpFields(json);
  } else if (isVoid()) {
    json.property("type", "void");
  } else {
    json.formatProperty("type", "Unknown(%zx)", size_t(asRawBits()));
  }
}

void JS::PropertyKey::dumpPropertyName(js::GenericPrinter& out) const {
  if (isAtom()) {
    toAtom()->dumpPropertyName(out);
  } else if (isInt()) {
    out.printf("%d", toInt());
  } else if (isSymbol()) {
    toSymbol()->dumpPropertyName(out);
  } else if (isVoid()) {
    out.put("(void)");
  } else {
    out.printf("Unknown(%zx)", size_t(asRawBits()));
  }
}

void JS::PropertyKey::dumpStringContent(js::GenericPrinter& out) const {
  if (isAtom()) {
    toAtom()->dumpStringContent(out);
  } else if (isInt()) {
    out.printf("%d", toInt());
  } else if (isSymbol()) {
    toSymbol()->dumpStringContent(out);
  } else if (isVoid()) {
    out.put("(void)");
  } else {
    out.printf("Unknown(%zx)", size_t(asRawBits()));
  }
}

#endif /* defined(DEBUG) || defined(JS_JITSPEW) */
