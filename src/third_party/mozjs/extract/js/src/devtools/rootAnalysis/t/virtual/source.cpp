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

// Special-cased function -- code that can run JS has an artificial edge to
// js::RunScript.
namespace js {
void RunScript() { GC(); }
}  // namespace js

struct Cell {
  int f;
} ANNOTATE("GC Thing");

extern void foo();

void bar() { GC(); }

typedef void (*func_t)();

class Base {
 public:
  int ANNOTATE("field annotation") dummy;
  virtual void someGC() ANNOTATE("Base pure virtual method") = 0;
  virtual void someGC(int) ANNOTATE("overloaded Base pure virtual method") = 0;
  virtual void sibGC() = 0;
  virtual void onBase() { bar(); }
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
  virtual void ANNOTATE("Super pure virtual") noneGC() = 0;
  virtual void allGC() = 0;
  virtual void onSuper() { asm(""); }
  void nonVirtualFunc() { asm(""); }
};

class Sub1 : public Super {
 public:
  void noneGC() override { foo(); }
  void someGC() override ANNOTATE("Sub1 override") ANNOTATE("second attr") {
    foo();
  }
  void someGC(int) override ANNOTATE("Sub1 override for int overload") {
    foo();
  }
  void allGC() override {
    foo();
    bar();
  }
  void sibGC() override { foo(); }
  void onBase() override { foo(); }
} ANNOTATE("CSU1") ANNOTATE("CSU2");

class Sub2 : public Super {
 public:
  void noneGC() override { foo(); }
  void someGC() override {
    foo();
    bar();
  }
  void someGC(int) override {
    foo();
    bar();
  }
  void allGC() override {
    foo();
    bar();
  }
  void sibGC() override { foo(); }
};

class Sibling : public Base {
 public:
  virtual void noneGC() { foo(); }
  void someGC() override {
    foo();
    bar();
  }
  void someGC(int) override {
    foo();
    bar();
  }
  virtual void allGC() {
    foo();
    bar();
  }
  void sibGC() override { bar(); }
};

class AutoSuppressGC {
 public:
  AutoSuppressGC() {}
  ~AutoSuppressGC() {}
} ANNOTATE("Suppress GC");

void use(Cell*) { asm(""); }

class nsISupports {
 public:
  virtual ANNOTATE("Can run script") void danger() { asm(""); }

  virtual ~nsISupports() = 0;
};

class nsIPrincipal : public nsISupports {
 public:
  ~nsIPrincipal() override{};
};

struct JSPrincipals {
  int debugToken;
  JSPrincipals() = default;
  virtual ~JSPrincipals() { GC(); }
};

class nsJSPrincipals : public nsIPrincipal, public JSPrincipals {
 public:
  void Release() { delete this; }
};

class SafePrincipals : public nsIPrincipal {
 public:
  ~SafePrincipals() { foo(); }
};

void f() {
  Sub1 s1;
  Sub2 s2;

  static Cell cell;
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
  {
    Cell* c12 = &cell;
    super->sibGC();
    use(c12);
  }

  Base* base = &s2;
  {
    Cell* c13 = &cell;
    base->sibGC();
    use(c13);
  }

  nsJSPrincipals pals;
  {
    Cell* c14 = &cell;
    nsISupports* p = &pals;
    p->danger();
    use(c14);
  }

  // Base defines, Sub1 overrides, static Super can call either.
  {
    Cell* c15 = &cell;
    super->onBase();
    use(c15);
  }

  {
    Cell* c16 = &cell;
    s2.someGC(7);
    use(c16);
  }

  {
    Cell* c17 = &cell;
    super->someGC(7);
    use(c17);
  }

  {
    nsJSPrincipals* princ = new nsJSPrincipals();
    Cell* c18 = &cell;
    delete princ;  // Can GC
    use(c18);
  }

  {
    nsJSPrincipals* princ = new nsJSPrincipals();
    nsISupports* supp = static_cast<nsISupports*>(princ);
    Cell* c19 = &cell;
    delete supp;  // Can GC
    use(c19);
  }

  {
    auto* safe = new SafePrincipals();
    Cell* c20 = &cell;
    delete safe;  // Cannot GC
    use(c20);
  }

  {
    auto* safe = new SafePrincipals();
    nsISupports* supp = static_cast<nsISupports*>(safe);
    Cell* c21 = &cell;
    delete supp;  // Compiler thinks destructor can GC.
    use(c21);
  }
}

template <typename Function>
void Call1(Function&& f) {
  f();
}

template <typename Function>
void Call2(Function&& f) {
  f();
}

void function_pointers() {
  Cell cell;

  {
    auto* f = GC;
    Cell* c22 = &cell;
    f();
    use(c22);
  }

  {
    auto* f = GC;
    auto*& g = f;
    Cell* c23 = &cell;
    g();
    use(c23);
  }

  {
    auto* f = GC;
    Call1([&] {
      Cell* c24 = &cell;
      f();
      use(c24);
    });
  }
}

// Use a separate function to test `mallocSizeOf` annotations. Bug 1872197:
// functions that are specialized on a lambda function and call that function
// will have that call get mixed up with other calls of lambdas defined within
// the same function.
void annotated_function_pointers() {
  Cell cell;

  // Variables with the specific name "mallocSizeOf" are
  // annotated to not GC. (Heh... even though here, they
  // *do* GC!)

  {
    auto* mallocSizeOf = GC;
    Cell* c25 = &cell;
    mallocSizeOf();
    use(c25);
  }

  {
    auto* f = GC;
    auto*& mallocSizeOf = f;
    Cell* c26 = &cell;
    mallocSizeOf();
    use(c26);
  }

  {
    auto* mallocSizeOf = GC;
    Call2([&] {
      Cell* c27 = &cell;
      mallocSizeOf();
      use(c27);
    });
  }
}
