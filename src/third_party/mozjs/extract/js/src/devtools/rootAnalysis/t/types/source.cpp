/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <memory>
#include <utility>

#define ANNOTATE(property) __attribute__((annotate(property)))

struct Cell {
  int f;
} ANNOTATE("GC Thing");

namespace World {
namespace NS {
struct Unsafe {
  int g;
  ~Unsafe() { asm(""); }
} ANNOTATE("Invalidated by GC") ANNOTATE("GC Pointer or Reference");
}  // namespace NS
}  // namespace World

extern void GC() ANNOTATE("GC Call");
extern void invisible();

void GC() {
  // If the implementation is too trivial, the function body won't be emitted at
  // all.
  asm("");
  invisible();
}

struct GCOnDestruction {
  ~GCOnDestruction() { GC(); }
};

struct NoGCOnDestruction {
  ~NoGCOnDestruction() { asm(""); }
};

extern void usecell(Cell*);

Cell* cell() {
  static Cell c;
  return &c;
}

template <typename T, typename U>
struct SimpleTemplate {
  int member;
};

template <typename T, typename U>
class ANNOTATE("moz_inherit_type_annotations_from_template_args") Container {
 public:
  template <typename V, typename W>
  void foo(V& v, W& w) {
    class InnerClass {};
    InnerClass xxx;
    return;
  }

  struct Entry {
    T t;
    U u;
  }* ent;
};

Cell* f() {
  Container<int, double> c1;
  Container<SimpleTemplate<int, int>, SimpleTemplate<double, double>> c2;
  Container<Container<int, double>, Container<float, float>> c3;
  Container<Container<SimpleTemplate<int, int>, float>,
            Container<float, SimpleTemplate<char, char>>>
      c4;

  return nullptr;
}

// Define a set of classes for verifying that there is no infinite loop
// when a class contains itself via mozilla::UniquePtr.

namespace mozilla {

template <typename A>
struct JustAField {
  A field;

  // Hack to allow UniquePtr and SimpleUniquePtr to be swapped.
  A& operator->() { return field; }
};

template <typename T>
struct UniquePtr {
  JustAField<T*> holder;
};

// This did not trigger the infinite loop, because the pointer here
// caused the UniquePtr special handling to be skipped. It requires
// the above definition to be triggered, which matches the actual
// implementation (JustAField maps to CompactPair, more or less).
// The bugfix for the infinite loop also drops this requirement, so
// now this *would* trigger the bug if it weren't fixed in the same
// commit.
template <typename T>
struct SimpleUniquePtr {
  T* holder;
};

}  // namespace mozilla

class Recursive {
 public:
  using EntryMap = Container<Cell*, Recursive>;
  mozilla::UniquePtr<EntryMap> entries;
};

void rvalue_ref(World::NS::Unsafe&& arg1) { GC(); }

void ref(const World::NS::Unsafe& arg2) {
  Recursive* foo;
  // Must actually use a type for the compiler to instantiate the
  // template specializations.
  foo->entries.holder->ent;
  GC();
  static int use = arg2.g;
}

// A function that consumes a parameter, but only if passed by rvalue reference.
extern void eat(World::NS::Unsafe&&);
extern void eat(World::NS::Unsafe&);

void rvalue_ref_ok() {
  World::NS::Unsafe unsafe1;
  eat(std::move(unsafe1));
  GC();
}

void rvalue_ref_not_ok() {
  World::NS::Unsafe unsafe2;
  eat(unsafe2);
  GC();
}

void rvalue_ref_arg_ok(World::NS::Unsafe&& unsafe3) {
  eat(std::move(unsafe3));
  GC();
}

void rvalue_ref_arg_not_ok(World::NS::Unsafe&& unsafe4) {
  eat(unsafe4);
  GC();
}

void shared_ptr_hazard() {
  Cell* unsafe5 = f();
  { auto p = std::make_shared<GCOnDestruction>(); }
  usecell(unsafe5);
}

void shared_ptr_no_hazard() {
  Cell* safe6 = f();
  { auto p = std::make_shared<NoGCOnDestruction>(); }
  usecell(safe6);
}
