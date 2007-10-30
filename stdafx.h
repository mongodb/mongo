// stdafx.h : include file for standard system include files,
// or project specific include files that are used frequently, but
// are changed infrequently
//

#pragma once

#include "targetver.h"

#include "assert.h"
#include <stdio.h>
#include <sstream>

//#if defined(_WIN32)
//#include <tchar.h>
//#else
typedef char _TCHAR;
//#endif

#include <iostream>
#include <fstream>
using namespace std;

#include <map>
#include <string>
#include <vector>

#if !defined(_WIN32)
typedef int HANDLE;
inline void strcpy_s(char *dst, unsigned len, const char *src) { strcpy(dst, src); }
#else
typedef void *HANDLE;
#endif

//#if defined(CHAR)
//#error CHAR already defined?
//#endif

//#if defined(_WIN32_WINNT)
//typedef wchar_t CHAR;
//#else
// more to be done...linux unicode is 32 bit.
//typedef unsigned short CHAR; // 16 bit unicode
//#endif

#define null (0)

#include <vector>
