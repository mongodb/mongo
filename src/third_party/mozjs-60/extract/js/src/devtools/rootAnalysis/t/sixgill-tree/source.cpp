#define ANNOTATE(property) __attribute__((tag(property)))

namespace js {
namespace gc {
struct Cell { int f; } ANNOTATE("GC Thing");
}
}

struct Bogon {
};

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
    JSObject *obj;
    void trace() {}
} ANNOTATE("Suppressed GC Pointer");

struct OkContainer {
    ErrorResult res;
    bool happy;
};

struct UnrootedPointer {
    JSObject *obj;
};

template <typename T>
class Rooted {
    T data;
} ANNOTATE("Rooted Pointer");

extern void js_GC() ANNOTATE("GC Call") ANNOTATE("Slow");

void js_GC() {}

void root_arg(JSObject *obj, JSObject *random)
{
  // Use all these types so they get included in the output.
  SpecialObject so;
  UnrootedPointer up;
  Bogon b;
  OkContainer okc;
  Rooted<JSObject*> ro;
  Rooted<SpecialObject*> rso;

  obj = random;

  JSObject *other1 = obj;
  js_GC();

  float MARKER1 = 0;
  JSObject *other2 = obj;
  other1->f = 1;
  other2->f = -1;

  unsigned int u1 = 1;
  unsigned int u2 = -1;
}
