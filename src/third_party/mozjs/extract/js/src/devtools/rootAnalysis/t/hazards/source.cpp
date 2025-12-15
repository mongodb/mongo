/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <utility>

#define ANNOTATE(property) __attribute__((annotate(property)))

// MarkVariableAsGCSafe is a magic function name used as an
// explicit annotation.

namespace JS {
namespace detail {
template <typename T>
static void MarkVariableAsGCSafe(T&) {
  asm("");
}
}  // namespace detail
}  // namespace JS

#define JS_HAZ_VARIABLE_IS_GC_SAFE(var) JS::detail::MarkVariableAsGCSafe(var)

struct Cell {
  int f;
} ANNOTATE("GC Thing");

template <typename T, typename U>
struct UntypedContainer {
  char data[sizeof(T) + sizeof(U)];
} ANNOTATE("moz_inherit_type_annotations_from_template_args");

struct RootedCell {
  RootedCell(Cell*) {}
} ANNOTATE("Rooted Pointer");

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

class AutoCheckCannotGC {
 public:
  AutoCheckCannotGC() {}
  ~AutoCheckCannotGC() { asm(""); }
} ANNOTATE("Invalidated by GC");

extern void GC() ANNOTATE("GC Call");
extern void invisible();

void GC() {
  // If the implementation is too trivial, the function body won't be emitted at
  // all.
  asm("");
  invisible();
}

extern Cell* makecell();

extern void usecell(Cell*);

extern bool flipcoin();

void suppressedFunction() {
  GC();  // Calls GC, but is always called within AutoSuppressGC
}

void halfSuppressedFunction() {
  GC();  // Calls GC, but is sometimes called within AutoSuppressGC
}

void unsuppressedFunction() {
  GC();  // Calls GC, never within AutoSuppressGC
}

class IDL_Interface {
 public:
  ANNOTATE("Can run script") virtual void canScriptThis() {}
  virtual void cannotScriptThis() {}
  ANNOTATE("Can run script") virtual void overridden_canScriptThis() = 0;
  virtual void overridden_cannotScriptThis() = 0;
};

class IDL_Subclass : public IDL_Interface {
  ANNOTATE("Can run script") void overridden_canScriptThis() override {}
  void overridden_cannotScriptThis() override {}
};

volatile static int x = 3;
volatile static int* xp = &x;
struct GCInDestructor {
  ~GCInDestructor() {
    invisible();
    asm("");
    *xp = 4;
    GC();
  }
};

template <typename T>
void usecontainer(T* value) {
  if (value) asm("");
}

Cell* cell() {
  static Cell c;
  return &c;
}

Cell* f() {
  GCInDestructor kaboom;

  Cell* cell1 = cell();
  Cell* cell2 = cell();
  Cell* cell3 = cell();
  Cell* cell4 = cell();
  {
    AutoSuppressGC nogc;
    suppressedFunction();
    halfSuppressedFunction();
  }
  usecell(cell1);
  halfSuppressedFunction();
  usecell(cell2);
  unsuppressedFunction();
  {
    // Old bug: it would look from the first AutoSuppressGC constructor it
    // found to the last destructor. This statement *should* have no effect.
    AutoSuppressGC nogc;
  }
  usecell(cell3);
  Cell* cell5 = cell();
  usecell(cell5);

  {
    // Templatized container that inherits attributes from Cell*, should
    // report a hazard.
    UntypedContainer<int, Cell*> container1;
    usecontainer(&container1);
    GC();
    usecontainer(&container1);
  }

  {
    // As above, but with a non-GC type.
    UntypedContainer<int, double> container2;
    usecontainer(&container2);
    GC();
    usecontainer(&container2);
  }

  // Hazard in return value due to ~GCInDestructor
  Cell* cell6 = cell();
  return cell6;
}

Cell* copy_and_gc(Cell* src) {
  GC();
  return reinterpret_cast<Cell*>(88);
}

void use(Cell* cell) {
  static int x = 0;
  if (cell) x++;
}

struct CellContainer {
  Cell* cell;
  CellContainer() { asm(""); }
};

void loopy() {
  Cell cell;

  // No hazard: haz1 is not live during call to copy_and_gc.
  Cell* haz1;
  for (int i = 0; i < 10; i++) {
    haz1 = copy_and_gc(haz1);
  }

  // No hazard: haz2 is live up to just before the GC, and starting at the
  // next statement after it, but not across the GC.
  Cell* haz2 = &cell;
  for (int j = 0; j < 10; j++) {
    use(haz2);
    GC();
    haz2 = &cell;
  }

  // Hazard: haz3 is live from the final statement in one iteration, across
  // the GC in the next, to the use in the 2nd statement.
  Cell* haz3;
  for (int k = 0; k < 10; k++) {
    GC();
    use(haz3);
    haz3 = &cell;
  }

  // Hazard: haz4 is live across a GC hidden in a loop.
  Cell* haz4 = &cell;
  for (int i2 = 0; i2 < 10; i2++) {
    GC();
  }
  use(haz4);

  // Hazard: haz5 is live from within a loop across a GC.
  Cell* haz5;
  for (int i3 = 0; i3 < 10; i3++) {
    haz5 = &cell;
  }
  GC();
  use(haz5);

  // No hazard: similar to the haz3 case, but verifying that we do not get
  // into an infinite loop.
  Cell* haz6;
  for (int i4 = 0; i4 < 10; i4++) {
    GC();
    haz6 = &cell;
  }

  // No hazard: haz7 is constructed within the body, so it can't make a
  // hazard across iterations. Note that this requires CellContainer to have
  // a constructor, because otherwise the analysis doesn't see where
  // variables are declared. (With the constructor, it knows that
  // construction of haz7 obliterates any previous value it might have had.
  // Not that that's possible given its scope, but the analysis doesn't get
  // that information.)
  for (int i5 = 0; i5 < 10; i5++) {
    GC();
    CellContainer haz7;
    use(haz7.cell);
    haz7.cell = &cell;
  }

  // Hazard: make sure we *can* see hazards across iterations involving
  // CellContainer;
  CellContainer haz8;
  for (int i6 = 0; i6 < 10; i6++) {
    GC();
    use(haz8.cell);
    haz8.cell = &cell;
  }
}

namespace mozilla {
template <typename T>
class UniquePtr {
  T* val;

 public:
  UniquePtr() : val(nullptr) { asm(""); }
  UniquePtr(T* p) : val(p) {}
  UniquePtr(UniquePtr<T>&& u) : val(u.val) { u.val = nullptr; }
  ~UniquePtr() { use(val); }
  T* get() { return val; }
  void reset() { val = nullptr; }
} ANNOTATE("moz_inherit_type_annotations_from_template_args");
}  // namespace mozilla

extern void consume(mozilla::UniquePtr<Cell> uptr);

void safevals() {
  Cell cell;

  // Simple hazard.
  Cell* unsafe1 = &cell;
  GC();
  use(unsafe1);

  // Safe because it's known to be nullptr.
  Cell* safe2 = &cell;
  safe2 = nullptr;
  GC();
  use(safe2);

  // Unsafe because it may not be nullptr.
  Cell* unsafe3 = &cell;
  if (reinterpret_cast<long>(&cell) & 0x100) {
    unsafe3 = nullptr;
  }
  GC();
  use(unsafe3);

  // Unsafe because it's not nullptr anymore.
  Cell* unsafe3b = &cell;
  unsafe3b = nullptr;
  unsafe3b = &cell;
  GC();
  use(unsafe3b);

  // Hazard involving UniquePtr.
  {
    mozilla::UniquePtr<Cell> unsafe4(&cell);
    GC();
    // Destructor uses unsafe4.
  }

  // reset() to safe value before the GC.
  {
    mozilla::UniquePtr<Cell> safe5(&cell);
    safe5.reset();
    GC();
  }

  // reset() to safe value after the GC.
  {
    mozilla::UniquePtr<Cell> safe6(&cell);
    GC();
    safe6.reset();
  }

  // reset() to safe value after the GC -- but we've already used it, so it's
  // too late.
  {
    mozilla::UniquePtr<Cell> unsafe7(&cell);
    GC();
    use(unsafe7.get());
    unsafe7.reset();
  }

  // initialized to safe value.
  {
    mozilla::UniquePtr<Cell> safe8;
    GC();
  }

  // passed to a function that takes ownership before GC.
  {
    mozilla::UniquePtr<Cell> safe9(&cell);
    consume(std::move(safe9));
    GC();
  }

  // passed to a function that takes ownership after GC.
  {
    mozilla::UniquePtr<Cell> unsafe10(&cell);
    GC();
    consume(std::move(unsafe10));
  }

  // annotated to be safe before the GC. (This doesn't make
  // a lot of sense here; the annotation is for when some
  // type is known to only contain safe values, eg it is
  // initialized as empty, or it is a union and we know
  // that the GC pointer variants are not in use.)
  {
    mozilla::UniquePtr<Cell> safe11(&cell);
    JS_HAZ_VARIABLE_IS_GC_SAFE(safe11);
    GC();
  }

  // annotate as safe value after the GC -- since nothing else
  // has touched the variable, that means it was already safe
  // during the GC.
  {
    mozilla::UniquePtr<Cell> safe12(&cell);
    GC();
    JS_HAZ_VARIABLE_IS_GC_SAFE(safe12);
  }

  // annotate as safe after the GC -- but we've already used it, so it's
  // too late.
  {
    mozilla::UniquePtr<Cell> unsafe13(&cell);
    GC();
    use(unsafe13.get());
    JS_HAZ_VARIABLE_IS_GC_SAFE(unsafe13);
  }

  // Check JS_HAZ_CAN_RUN_SCRIPT annotation handling.
  IDL_Subclass sub;
  IDL_Subclass* subp = &sub;
  IDL_Interface* base = &sub;
  {
    Cell* unsafe14 = &cell;
    base->canScriptThis();
    use(unsafe14);
  }
  {
    Cell* unsafe15 = &cell;
    subp->canScriptThis();
    use(unsafe15);
  }
  {
    // Almost the same as the last one, except call using the actual object, not
    // a pointer. The type is known, so there is no danger of the actual type
    // being a subclass that has overridden the method with an implementation
    // that calls script.
    Cell* safe16 = &cell;
    sub.canScriptThis();
    use(safe16);
  }
  {
    Cell* safe17 = &cell;
    base->cannotScriptThis();
    use(safe17);
  }
  {
    Cell* safe18 = &cell;
    subp->cannotScriptThis();
    use(safe18);
  }
  {
    // A use after a GC, but not before. (This does not initialize safe19 by
    // setting it to a value, because assignment would start its live range, and
    // this test is to see if a variable with no known live range start requires
    // a use before the GC or not. It should.)
    Cell* safe19;
    GC();
    extern void initCellPtr(Cell**);
    initCellPtr(&safe19);
  }
}

// Make sure `this` is live at the beginning of a function.
class Subcell : public Cell {
  int method() {
    GC();
    return f;  // this->f
  }
};

template <typename T>
struct RefPtr {
  ~RefPtr() { GC(); }
  bool forget() { return true; }
  bool use() { return true; }
  void assign_with_AddRef(T* aRawPtr) { asm(""); }
};

extern bool flipcoin();

Cell* refptr_test1() {
  static Cell cell;
  RefPtr<float> v1;
  Cell* ref_unsafe1 = &cell;
  return ref_unsafe1;
}

Cell* refptr_test2() {
  static Cell cell;
  RefPtr<float> v2;
  Cell* ref_safe2 = &cell;
  v2.forget();
  return ref_safe2;
}

Cell* refptr_test3() {
  static Cell cell;
  RefPtr<float> v3;
  Cell* ref_unsafe3 = &cell;
  if (x) {
    v3.forget();
  }
  return ref_unsafe3;
}

Cell* refptr_test4() {
  static Cell cell;
  RefPtr<int> r;
  return &cell;  // hazard in return value
}

Cell* refptr_test5() {
  static Cell cell;
  RefPtr<int> r;
  return nullptr;  // returning immobile value, so no hazard
}

float somefloat = 1.2;

Cell* refptr_test6() {
  static Cell cell;
  RefPtr<float> v6;
  Cell* ref_unsafe6 = &cell;
  // v6 can be used without an intervening forget() before the end of the
  // function, even though forget() will be called at least once.
  v6.forget();
  if (x) {
    v6.forget();
    v6.assign_with_AddRef(&somefloat);
  }
  return ref_unsafe6;
}

Cell* refptr_test7() {
  static Cell cell;
  RefPtr<float> v7;
  Cell* ref_unsafe7 = &cell;
  // Similar to above, but with a loop.
  while (flipcoin()) {
    v7.forget();
    v7.assign_with_AddRef(&somefloat);
  }
  return ref_unsafe7;
}

Cell* refptr_test8() {
  static Cell cell;
  RefPtr<float> v8;
  Cell* ref_unsafe8 = &cell;
  // If the loop is traversed, forget() will be called. But that doesn't
  // matter, because even on the last iteration v8.use() will have been called
  // (and potentially dropped the refcount or whatever.)
  while (v8.use()) {
    v8.forget();
  }
  return ref_unsafe8;
}

Cell* refptr_test9() {
  static Cell cell;
  RefPtr<float> v9;
  Cell* ref_safe9 = &cell;
  // Even when not going through the loop, forget() will be called and so the
  // dtor will not Release.
  while (v9.forget()) {
    v9.assign_with_AddRef(&somefloat);
  }
  return ref_safe9;
}

Cell* refptr_test10() {
  static Cell cell;
  RefPtr<float> v10;
  Cell* ref_unsafe10 = &cell;
  // The destructor has a backwards path that skips the loop body.
  v10.assign_with_AddRef(&somefloat);
  while (flipcoin()) {
    v10.forget();
  }
  return ref_unsafe10;
}

std::pair<bool, AutoCheckCannotGC> pair_returning_function() {
  return std::make_pair(true, AutoCheckCannotGC());
}

void aggr_init_unsafe() {
  // nogc will be live after the call, so across the GC.
  auto [ok, nogc] = pair_returning_function();
  GC();
}

void aggr_init_safe() {
  // The analysis should be able to tell that nogc is only live after the call,
  // not before. (This is to check for a problem where the return value was
  // getting stored into a different temporary than the local nogc variable,
  // and so its initialization was never seen and so it was assumed to be live
  // throughout the function.)
  GC();
  auto [ok, nogc] = pair_returning_function();
}

void stack_array() {
  Cell* array[] = {makecell(), makecell()};
  Cell* array2[] = {makecell(), makecell()};
  GC();
  usecell(array[1]);
  // Never use array2.
}
