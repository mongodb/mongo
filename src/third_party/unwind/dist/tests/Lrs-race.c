/* libunwind - a platform-independent unwind library
   Copyright (C) 2003-2005 Hewlett-Packard Co
	Contributed by Paul Pluzhnikov <ppluzhnikov@google.com>

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.  */

/*  Verify that register state caches work under all caching policies
    in a multi-threaded environment with a large number IPs */

#define UNW_LOCAL_ONLY
#include <libunwind.h>
#include "compiler.h"

#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>

/* ITERS=1000, NTHREAD=10 caught some bugs in the past */
#ifndef ITERS 
#define ITERS 100
#endif

#ifndef NTHREAD
#define NTHREAD 2
#endif

int verbose;

void
foo_0 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_1 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_2 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_3 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_4 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_5 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_6 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_7 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_8 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_9 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_10 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_11 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_12 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_13 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_14 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_15 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_16 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_17 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_18 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_19 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_20 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_21 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_22 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_23 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_24 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_25 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_26 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_27 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_28 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_29 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_30 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_31 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_32 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_33 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_34 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_35 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_36 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_37 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_38 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_39 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_40 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_41 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_42 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_43 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_44 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_45 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_46 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_47 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_48 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_49 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_50 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_51 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_52 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_53 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_54 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_55 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_56 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_57 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_58 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_59 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_60 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_61 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_62 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_63 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_64 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_65 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_66 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_67 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_68 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_69 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_70 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_71 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_72 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_73 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_74 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_75 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_76 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_77 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_78 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_79 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_80 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_81 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_82 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_83 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_84 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_85 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_86 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_87 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_88 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_89 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_90 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_91 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_92 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_93 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_94 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_95 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_96 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_97 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_98 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_99 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_100 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_101 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_102 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_103 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_104 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_105 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_106 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_107 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_108 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_109 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_110 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_111 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_112 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_113 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_114 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_115 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_116 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_117 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_118 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_119 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_120 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_121 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_122 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_123 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_124 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_125 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_126 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_127 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void
foo_128 (void)
{
  void *buf[20];
  int n;

  if ((n = unw_backtrace (buf, 20)) < 3)
    abort ();
}

void *
bar(void *p UNUSED)
{
  int i;
  for (i = 0; i < ITERS; ++i) {
    foo_0 ();
    foo_1 ();
    foo_2 ();
    foo_3 ();
    foo_4 ();
    foo_5 ();
    foo_6 ();
    foo_7 ();
    foo_8 ();
    foo_9 ();
    foo_10 ();
    foo_11 ();
    foo_12 ();
    foo_13 ();
    foo_14 ();
    foo_15 ();
    foo_16 ();
    foo_17 ();
    foo_18 ();
    foo_19 ();
    foo_20 ();
    foo_21 ();
    foo_22 ();
    foo_23 ();
    foo_24 ();
    foo_25 ();
    foo_26 ();
    foo_27 ();
    foo_28 ();
    foo_29 ();
    foo_30 ();
    foo_31 ();
    foo_32 ();
    foo_33 ();
    foo_34 ();
    foo_35 ();
    foo_36 ();
    foo_37 ();
    foo_38 ();
    foo_39 ();
    foo_40 ();
    foo_41 ();
    foo_42 ();
    foo_43 ();
    foo_44 ();
    foo_45 ();
    foo_46 ();
    foo_47 ();
    foo_48 ();
    foo_49 ();
    foo_50 ();
    foo_51 ();
    foo_52 ();
    foo_53 ();
    foo_54 ();
    foo_55 ();
    foo_56 ();
    foo_57 ();
    foo_58 ();
    foo_59 ();
    foo_60 ();
    foo_61 ();
    foo_62 ();
    foo_63 ();
    foo_64 ();
    foo_65 ();
    foo_66 ();
    foo_67 ();
    foo_68 ();
    foo_69 ();
    foo_70 ();
    foo_71 ();
    foo_72 ();
    foo_73 ();
    foo_74 ();
    foo_75 ();
    foo_76 ();
    foo_77 ();
    foo_78 ();
    foo_79 ();
    foo_80 ();
    foo_81 ();
    foo_82 ();
    foo_83 ();
    foo_84 ();
    foo_85 ();
    foo_86 ();
    foo_87 ();
    foo_88 ();
    foo_89 ();
    foo_90 ();
    foo_91 ();
    foo_92 ();
    foo_93 ();
    foo_94 ();
    foo_95 ();
    foo_96 ();
    foo_97 ();
    foo_98 ();
    foo_99 ();
    foo_100 ();
    foo_101 ();
    foo_102 ();
    foo_103 ();
    foo_104 ();
    foo_105 ();
    foo_106 ();
    foo_107 ();
    foo_108 ();
    foo_109 ();
    foo_110 ();
    foo_111 ();
    foo_112 ();
    foo_113 ();
    foo_114 ();
    foo_115 ();
    foo_116 ();
    foo_117 ();
    foo_118 ();
    foo_119 ();
    foo_120 ();
    foo_121 ();
    foo_122 ();
    foo_123 ();
    foo_124 ();
    foo_125 ();
    foo_126 ();
    foo_127 ();
    foo_128 ();
  }
  return NULL;
}

int doit (void)
{
  pthread_t tid[NTHREAD];
  int i;

  for (i = 0; i < NTHREAD; ++i)
    if (pthread_create (&tid[i], NULL, bar, NULL))
      return 1;

  for (i = 0; i < NTHREAD; ++i)
    if (pthread_join (tid[i], NULL))
      return 1;

  return 0;
}

int
main (int argc, char **argv UNUSED)
{
  if (argc > 1)
    verbose = 1;

  if (verbose)
    printf ("Caching: none\n");
  unw_set_caching_policy (unw_local_addr_space, UNW_CACHE_NONE);
  doit ();

  if (verbose)
    printf ("Caching: global\n");
  unw_set_caching_policy (unw_local_addr_space, UNW_CACHE_GLOBAL);
  doit ();

  if (verbose)
    printf ("Caching: per-thread\n");
  unw_set_caching_policy (unw_local_addr_space, UNW_CACHE_PER_THREAD);
  doit ();

  if (verbose)
    printf ("SUCCESS\n");
  return 0;
}
