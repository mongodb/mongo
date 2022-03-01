/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#define ANNOTATE(property) __attribute__((annotate(property)))

namespace js {
namespace gc {
struct Cell {
  int f;
} ANNOTATE("GC Thing");
}  // namespace gc
}  // namespace js

struct Bogon {};

struct JustACell : public js::gc::Cell {
  bool iHaveNoDataMembers() { return true; }
};

struct JSObject : public js::gc::Cell, public Bogon {
  int g;
};

struct SpecialObject : public JSObject {
  int z;
};

struct ErrorResult {
  bool hasObj;
  JSObject* obj;
  void trace() {}
} ANNOTATE("Suppressed GC Pointer");

struct OkContainer {
  ErrorResult res;
  bool happy;
};

struct UnrootedPointer {
  JSObject* obj;
};

template <typename T>
class Rooted {
  T data;
} ANNOTATE("Rooted Pointer");

extern void js_GC() ANNOTATE("GC Call") ANNOTATE("Slow");

void js_GC() {}

void root_arg(JSObject* obj, JSObject* random) {
  // Use all these types so they get included in the output.
  SpecialObject so;
  UnrootedPointer up;
  Bogon b;
  OkContainer okc;
  Rooted<JSObject*> ro;
  Rooted<SpecialObject*> rso;

  obj = random;

  JSObject* other1 = obj;
  js_GC();

  float MARKER1 = 0;
  JSObject* other2 = obj;
  other1->f = 1;
  other2->f = -1;

  unsigned int u1 = 1;
  unsigned int u2 = -1;
}
