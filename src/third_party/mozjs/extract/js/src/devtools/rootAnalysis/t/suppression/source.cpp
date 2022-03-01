/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#define ANNOTATE(property) __attribute__((annotate(property)))

struct Cell {
  int f;
} ANNOTATE("GC Thing");

class AutoSuppressGC_Base {
 public:
  AutoSuppressGC_Base() {}
  ~AutoSuppressGC_Base() {}
} ANNOTATE("Suppress GC");

class AutoSuppressGC_Child : public AutoSuppressGC_Base {
 public:
  AutoSuppressGC_Child() : AutoSuppressGC_Base() {}
};

class AutoSuppressGC {
  AutoSuppressGC_Child helpImBeingSuppressed;

 public:
  AutoSuppressGC() {}
};

extern void GC() ANNOTATE("GC Call");

void GC() {
  // If the implementation is too trivial, the function body won't be emitted at
  // all.
  asm("");
}

extern void foo(Cell*);

void suppressedFunction() {
  GC();  // Calls GC, but is always called within AutoSuppressGC
}

void halfSuppressedFunction() {
  GC();  // Calls GC, but is sometimes called within AutoSuppressGC
}

void unsuppressedFunction() {
  GC();  // Calls GC, never within AutoSuppressGC
}

void f() {
  Cell* cell1 = nullptr;
  Cell* cell2 = nullptr;
  Cell* cell3 = nullptr;
  {
    AutoSuppressGC nogc;
    suppressedFunction();
    halfSuppressedFunction();
  }
  foo(cell1);
  halfSuppressedFunction();
  foo(cell2);
  unsuppressedFunction();
  {
    // Old bug: it would look from the first AutoSuppressGC constructor it
    // found to the last destructor. This statement *should* have no effect.
    AutoSuppressGC nogc;
  }
  foo(cell3);
}
