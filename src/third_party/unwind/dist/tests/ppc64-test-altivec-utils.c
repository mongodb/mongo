#include <stdio.h>
#include <altivec.h>

union si_overlay
{
  vector signed int v;
  int ints[4];
};

vector signed int
vec_init ()
{
  vector signed int v;
  static int count = 1;

  ((union si_overlay *) &v)->ints[0] = count++;
  ((union si_overlay *) &v)->ints[1] = count++;
  ((union si_overlay *) &v)->ints[2] = count++;
  ((union si_overlay *) &v)->ints[3] = count++;
  return v;
}

void
vec_print (vector signed int v)
{
  printf ("%08x %08x %08x %08x",
	 ((union si_overlay *) &v)->ints[0],
	 ((union si_overlay *) &v)->ints[1],
	 ((union si_overlay *) &v)->ints[2],
	 ((union si_overlay *) &v)->ints[3]);
}

