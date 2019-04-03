#include <stdio.h>

/* To prevent inlining and optimizing away */
int foo(volatile int* f) {
  return *f;
}
