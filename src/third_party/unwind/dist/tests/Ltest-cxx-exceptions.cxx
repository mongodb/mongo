/* libunwind - a platform-independent unwind library
   Copyright (C) 2010 stefan.demharter@gmx.net
   Copyright (C) 2010 arun.sharma@google.com

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

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <libunwind.h>
#include "compiler.h"

#define panic(args...)				\
	{ fprintf (stderr, args); exit (-1); }

static int verbose;

struct Test
{
  public: // --- ctor/dtor ---
    Test() { ++counter_; }
    ~Test() { -- counter_; }
    Test(const Test&) { ++counter_; }

  public: // --- static members ---
    static int counter_;
};

int Test::counter_ = 0;

// Called by foo
extern "C" void bar()
{
  Test t;
  try {
    Test t;
    throw 5;
  } catch (...) {
    Test t;
    if (verbose)
      printf("Throwing an int\n");
    throw 6;
  }
}

int main(int argc, char **argv UNUSED)
{
  if (argc > 1)
    verbose = 1;
  try {
    Test t;
    bar();
  } catch (int) {
    // Dtor of all Test-object has to be called.
    if (Test::counter_ != 0)
      panic("Counter non-zero\n");
    return Test::counter_;
  } catch (...) {
    // An int was thrown - we should not get here.
    panic("Int was thrown why are we here?\n");
  }
  exit(0);
}
