// Copyright 2002 and onwards Google Inc.
//
// Printf variants that place their output in a C++ string.
//
// Usage:
//      string result = StringPrintf("%d %s\n", 10, "hello");
//      SStringPrintf(&result, "%d %s\n", 10, "hello");
//      StringAppendF(&result, "%d %s\n", 20, "there");

#ifndef _STRINGS_STRINGPRINTF_H
#define _STRINGS_STRINGPRINTF_H

#include <stdarg.h>
#include <string>
using std::string;

#include <vector>
using std::vector;

#include "base/port.h"
#include "base/stringprintf.h"

// This file formerly contained
//   StringPrintf, SStringPrintf, StringAppendF, and StringAppendV.
// These routines have moved to base/stringprintf.{h,cc} to allow
// using them from files in base.  We include base/stringprintf.h
// in this file since so many clients were dependent on these
// routines being defined in stringprintf.h.


// The max arguments supported by StringPrintfVector
extern const int kStringPrintfVectorMaxArgs;

// You can use this version when all your arguments are strings, but
// you don't know how many arguments you'll have at compile time.
// StringPrintfVector will LOG(FATAL) if v.size() > kStringPrintfVectorMaxArgs
extern string StringPrintfVector(const char* format, const vector<string>& v);

#endif /* _STRINGS_STRINGPRINTF_H */
