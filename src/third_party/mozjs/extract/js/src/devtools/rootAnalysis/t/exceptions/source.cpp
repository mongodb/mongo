/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Simply including <exception> was enough to crash sixgill at one point.
#include <exception>

#define ANNOTATE(property) __attribute__((annotate(property)))

struct Cell {
  int f;
} ANNOTATE("GC Thing");

extern void GC() ANNOTATE("GC Call");

void GC() {
  // If the implementation is too trivial, the function body won't be emitted at
  // all.
  asm("");
}

class RAII_GC {
 public:
  RAII_GC() {}
  ~RAII_GC() { GC(); }
};

// ~AutoSomething calls GC because of the RAII_GC field. The constructor,
// though, should *not* GC -- unless it throws an exception. Which is not
// possible when compiled with -fno-exceptions. This test will try it both
// ways.
class AutoSomething {
  RAII_GC gc;

 public:
  AutoSomething() : gc() {
    asm("");  // Ooh, scary, this might throw an exception
  }
  ~AutoSomething() { asm(""); }
};

extern Cell* getcell();

extern void usevar(Cell* cell);

void f() {
  Cell* thing = getcell();  // Live range starts here

  // When compiling with -fexceptions, there should be a hazard below. With
  // -fno-exceptions, there should not be one. We will check both.
  {
    AutoSomething smth;  // Constructor can GC only if exceptions are enabled
    usevar(thing);       // Live range ends here
  }  // In particular, 'thing' is dead at the destructor, so no hazard
}
