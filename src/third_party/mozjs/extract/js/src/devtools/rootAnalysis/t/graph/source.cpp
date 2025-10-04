#define ANNOTATE(property) __attribute__((annotate(property)))

extern void GC() ANNOTATE("GC Call");

void GC() {
  // If the implementation is too trivial, the function body won't be emitted at
  // all.
  asm("");
}

extern void g(int x);
extern void h(int x);

void f(int x) {
  if (x % 3) {
    GC();
    g(x);
  }
  h(x);
}

void g(int x) {
  if (x % 2) f(x);
  h(x);
}

void h(int x) {
  if (x) {
    f(x - 1);
    g(x - 1);
  }
}

void leaf() { asm(""); }

void nonrecursive_root() {
  leaf();
  leaf();
  GC();
}

void self_recursive(int x) {
  if (x) self_recursive(x - 1);
}

// Set up the graph
//
//   n1 <--> n2          n4 <--> n5
//           \                  /
//            --> n3 <---------
//                 \
//                  ---> n6 --> n7 <---> n8 --> n9
//
// So recursive roots are one of (n1, n2) plus one of (n4, n5).
extern void n1(int x);
extern void n2(int x);
extern void n3(int x);
extern void n4(int x);
extern void n5(int x);
extern void n6(int x);
extern void n7(int x);
extern void n8(int x);
extern void n9(int x);

void n1(int x) { n2(x); }

void n2(int x) {
  if (x) n1(x - 1);
  n3(x);
}

void n4(int x) { n5(x); }

void n5(int x) {
  if (x) n4(x - 1);
  n3(x);
}

void n3(int x) { n6(x); }

void n6(int x) { n7(x); }

void n7(int x) { n8(x); }

void n8(int x) {
  if (x) n7(x - 1);
  n9(x);
}

void n9(int x) { asm(""); }
