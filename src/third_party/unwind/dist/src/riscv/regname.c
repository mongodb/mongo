/* libunwind - a platform-independent unwind library
   Copyright (C) 2008 CodeSourcery
   Copyright (C) 2014 Tilera Corp.

This file is part of libunwind.

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

#include "unwind_i.h"

static const char *regname[] =
  {
    /* 0.  */
    "zero",  "ra",  "sp",  "gp",  "tp",  "t0",  "t1",  "t2",
    /* 8.  */
    "s0",  "s1",  "a0",  "a1",  "a2",  "a3",  "a4",  "a5",
    /* 16.  */
    "a6",  "a7",  "s2",  "s3",  "s4",  "s5",  "s6",  "s7",
    /* 24.  */
    "s8",  "s9",  "s10",  "s11",  "t3",  "t4",  "t5",  "t6",

    /* 0.  */
    "f0",  "f1",  "f2",  "f3",  "f4",  "f5",  "f6",  "f7",
    /* 8.  */
    "f8",  "f9",  "f10",  "f11",  "f12",  "f13",  "f14",  "f15",
    /* 16.  */
    "f16",  "f17",  "f18",  "f19",  "f20",  "f21",  "f22",  "f23",
    /* 24.  */
    "f24",  "f25",  "f26",  "f27",  "f28",  "f29",  "f30",  "f31",

    /* pc */
    "pc"
  };

const char *
unw_regname (unw_regnum_t reg)
{
  if (reg < (unw_regnum_t) ARRAY_SIZE (regname))
    return regname[reg];
  else
    return "???";
}
