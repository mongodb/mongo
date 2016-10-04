// Copyright 2002 and onwards Google Inc.

#include <stdarg.h> // For va_list and related operations
#include <stdio.h> // MSVC requires this for _vsnprintf
#include <vector>
using std::vector;

#include <string>
using std::string;
#include "stringprintf.h"
#include "base/logging.h"
// Max arguments supported by StringPrintVector
const int kStringPrintfVectorMaxArgs = 32;

// An empty block of zero for filler arguments.  This is const so that if
// printf tries to write to it (via %n) then the program gets a SIGSEGV
// and we can fix the problem or protect against an attack.
static const char string_printf_empty_block [256] = { '\0' };

string StringPrintfVector(const char* format, const vector<string>& v) {
  CHECK_LE(v.size(), static_cast<size_t>(kStringPrintfVectorMaxArgs))
      << "StringPrintfVector currently only supports up to "
      << kStringPrintfVectorMaxArgs << " arguments. "
      << "Feel free to add support for more if you need it.";

  // Add filler arguments so that bogus format+args have a harder time
  // crashing the program, corrupting the program (%n),
  // or displaying random chunks of memory to users.

  const char* cstr[kStringPrintfVectorMaxArgs];
  for (size_t i = 0; i < v.size(); ++i) {
    cstr[i] = v[i].c_str();
  }
  for (size_t i = v.size(); i < arraysize(cstr); ++i) {
    cstr[i] = &string_printf_empty_block[0];
  }

  // I do not know any way to pass kStringPrintfVectorMaxArgs arguments,
  // or any way to build a va_list by hand, or any API for printf
  // that accepts an array of arguments.  The best I can do is stick
  // this COMPILE_ASSERT right next to the actual statement.

  COMPILE_ASSERT(kStringPrintfVectorMaxArgs == 32, arg_count_mismatch);
  return StringPrintf(format,
                      cstr[0], cstr[1], cstr[2], cstr[3], cstr[4],
                      cstr[5], cstr[6], cstr[7], cstr[8], cstr[9],
                      cstr[10], cstr[11], cstr[12], cstr[13], cstr[14],
                      cstr[15], cstr[16], cstr[17], cstr[18], cstr[19],
                      cstr[20], cstr[21], cstr[22], cstr[23], cstr[24],
                      cstr[25], cstr[26], cstr[27], cstr[28], cstr[29],
                      cstr[30], cstr[31]);
}
