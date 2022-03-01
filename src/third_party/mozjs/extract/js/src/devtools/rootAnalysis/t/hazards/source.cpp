/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <utility>

#define ANNOTATE(property) __attribute__((annotate(property)))

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

extern void GC() ANNOTATE("GC Call");
extern void invisible();

void GC() {
  // If the implementation is too trivial, the function body won't be emitted at
  // all.
  asm("");
  invisible();
}

extern void usecell(Cell*);

void suppressedFunction() {
  GC();  // Calls GC, but is always called within AutoSuppressGC
}

void halfSuppressedFunction() {
  GC();  // Calls GC, but is sometimes called within AutoSuppressGC
}

void unsuppressedFunction() {
  GC();  // Calls GC, never within AutoSuppressGC
}

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
}

// Make sure `this` is live at the beginning of a function.
class Subcell : public Cell {
  int method() {
    GC();
    return f;  // this->f
  }
};
