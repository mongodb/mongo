/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#define ANNOTATE(property) __attribute__((annotate(property)))

extern void GC() ANNOTATE("GC Call");

void GC() {
  // If the implementation is too trivial, the function body won't be emitted at
  // all.
  asm("");
}

struct Cell {
  int f;
} ANNOTATE("GC Thing");

extern void foo();

typedef void (*func_t)();

class Base {
 public:
  int ANNOTATE("field annotation") dummy;
  virtual void someGC() ANNOTATE("Base pure virtual method") = 0;
  func_t functionField;

  // For now, this is just to verify that the plugin doesn't crash. The
  // analysis code does not yet look at this annotation or output it anywhere
  // (though it *is* being recorded.)
  static float testAnnotations() ANNOTATE("static func");

  // Similar, though sixgill currently completely ignores parameter annotations.
  static double testParamAnnotations(Cell& ANNOTATE("param annotation")
                                         ANNOTATE("second param annot") cell)
      ANNOTATE("static func") ANNOTATE("second func");
};

float Base::testAnnotations() {
  asm("");
  return 1.1;
}

double Base::testParamAnnotations(Cell& cell) {
  asm("");
  return 1.2;
}

class Super : public Base {
 public:
  virtual void noneGC() = 0;
  virtual void allGC() = 0;
};

void bar() { GC(); }

class Sub1 : public Super {
 public:
  void noneGC() override { foo(); }
  void someGC() override ANNOTATE("Sub1 override") ANNOTATE("second attr") {
    foo();
  }
  void allGC() override {
    foo();
    bar();
  }
} ANNOTATE("CSU1") ANNOTATE("CSU2");

class Sub2 : public Super {
 public:
  void noneGC() override { foo(); }
  void someGC() override {
    foo();
    bar();
  }
  void allGC() override {
    foo();
    bar();
  }
};

class Sibling : public Base {
 public:
  virtual void noneGC() { foo(); }
  void someGC() override {
    foo();
    bar();
  }
  virtual void allGC() {
    foo();
    bar();
  }
};

class AutoSuppressGC {
 public:
  AutoSuppressGC() {}
  ~AutoSuppressGC() {}
} ANNOTATE("Suppress GC");

void use(Cell*) { asm(""); }

void f() {
  Sub1 s1;
  Sub2 s2;

  Cell cell;
  {
    Cell* c1 = &cell;
    s1.noneGC();
    use(c1);
  }
  {
    Cell* c2 = &cell;
    s2.someGC();
    use(c2);
  }
  {
    Cell* c3 = &cell;
    s1.allGC();
    use(c3);
  }
  {
    Cell* c4 = &cell;
    s2.noneGC();
    use(c4);
  }
  {
    Cell* c5 = &cell;
    s2.someGC();
    use(c5);
  }
  {
    Cell* c6 = &cell;
    s2.allGC();
    use(c6);
  }

  Super* super = &s2;
  {
    Cell* c7 = &cell;
    super->noneGC();
    use(c7);
  }
  {
    Cell* c8 = &cell;
    super->someGC();
    use(c8);
  }
  {
    Cell* c9 = &cell;
    super->allGC();
    use(c9);
  }

  {
    Cell* c10 = &cell;
    s1.functionField();
    use(c10);
  }
  {
    Cell* c11 = &cell;
    super->functionField();
    use(c11);
  }
}
