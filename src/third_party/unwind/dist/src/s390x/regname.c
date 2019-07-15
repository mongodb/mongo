/* libunwind - a platform-independent unwind library

        Contributed by Max Asbock <masbock@us.ibm.com>

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
    [UNW_S390X_R0]="R0",
    [UNW_S390X_R1]="R1",
    [UNW_S390X_R2]="R2",
    [UNW_S390X_R3]="R3",
    [UNW_S390X_R4]="R4",
    [UNW_S390X_R5]="R5",
    [UNW_S390X_R6]="R6",
    [UNW_S390X_R7]="R7",
    [UNW_S390X_R8]="R8",
    [UNW_S390X_R9]="R9",
    [UNW_S390X_R10]="R10",
    [UNW_S390X_R11]="R11",
    [UNW_S390X_R12]="R12",
    [UNW_S390X_R13]="R13",
    [UNW_S390X_R14]="R14",
    [UNW_S390X_R15]="R15",

    [UNW_S390X_IP]="IP"
   };

const char *
unw_regname (unw_regnum_t reg)
{
  if (reg < (unw_regnum_t) ARRAY_SIZE (regname))
    return regname[reg];
  else
    return "???";
}
