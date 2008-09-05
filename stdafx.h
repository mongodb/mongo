// stdafx.h : include file for standard system include files,
// or project specific include files that are used frequently, but
// are changed infrequently
//

/**
*    Copyright (C) 2008 10gen Inc.
*  
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*  
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*  
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#if defined(_WIN32)
const bool debug=true;
#else
const bool debug=false;
#endif

#include <memory>

extern void dbexit(int returnCode, const char *whyMsg = "");

inline void * ourmalloc(size_t size) { 
	void *x = malloc(size);
	if( x == 0 ) dbexit(42, "malloc fails");
	return x;
}

inline void * ourrealloc(void *ptr, size_t size) { 
	void *x = realloc(ptr, size);
	if( x == 0 ) dbexit(43, "realloc fails");
	return x;
}

#define malloc ourmalloc
#define realloc ourrealloc

#include "targetver.h"

//#include "assert.h"

// you can catch these
class AssertionException { 
public:
	const char *msg;
	AssertionException() { msg = ""; }
	virtual bool isUserAssertion() { return false; }
};

/* we use the same mechanism for bad things the user does -- which are really just errors */
class UserAssertionException : public AssertionException { 
public:
	UserAssertionException(const char *_msg) { msg = _msg; }
	virtual bool isUserAssertion() { return true; }
};

void asserted(const char *msg, const char *file, unsigned line);
void wasserted(const char *msg, const char *file, unsigned line);
void uasserted(const char *msg);
void msgasserted(const char *msg); 

#ifdef assert
#undef assert
#endif

#define assert(_Expression) (void)( (!!(_Expression)) || (asserted(#_Expression, __FILE__, __LINE__), 0) )

/* "user assert".  if asserts, user did something wrong, not our code */
//#define uassert(_Expression) (void)( (!!(_Expression)) || (uasserted(#_Expression, __FILE__, __LINE__), 0) )
#define uassert(msg,_Expression) (void)( (!!(_Expression)) || (uasserted(msg), 0) )

#define xassert(_Expression) (void)( (!!(_Expression)) || (asserted(#_Expression, __FILE__, __LINE__), 0) )

#define yassert 1

/* warning only - keeps going */
#define wassert(_Expression) (void)( (!!(_Expression)) || (wasserted(#_Expression, __FILE__, __LINE__), 0) )

/* display a message, no context, and throw assertionexception

   easy way to throw an exception and log something without our stack trace 
   display happening.
*/
#define massert(msg,_Expression) (void)( (!!(_Expression)) || (msgasserted(msg), 0) )

/* dassert is 'debug assert' -- might want to turn off for production as these 
   could be slow.
*/
#define dassert assert

#include <stdio.h>
#include <sstream>
#include <signal.h>

typedef char _TCHAR;

#include <iostream>
#include <fstream>
using namespace std;

#include "time.h"
#include <map>
#include <string>
#include <vector>
#include <set>

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

// for debugging
typedef struct _Ints { int i[100]; } *Ints;
typedef struct _Chars { char c[200]; } *Chars;

typedef char CHARS[400];

typedef struct _OWS {
	int size;
	char type;
	char string[400];
} *OWS;

class Client;
extern Client *client;
extern const char *curNs;

/* for now, running on win32 means development not production -- 
   use this to log things just there.
*/
#if defined(_WIN32)
#define DEV if( 0 ) 
#else
#define DEV if( 0 ) 
#endif

#define DEBUGGING if( 0 ) 

extern unsigned occasion; 

#define OCCASIONALLY if( ++occasion % 16 == 0 ) 
#define RARELY if( ++occasion % 128 == 0 ) 

#if defined(_WIN32)
inline void our_debug_free(void *p) {
	unsigned *u = (unsigned *) p;
	u[0] = 0xEEEEEEEE;
	u[1] = 0xEEEEEEEE;
	free(p);
}
#define free our_debug_free
#endif

#define exit dbexit

#undef yassert
#include <boost/filesystem/convenience.hpp>
#include <boost/filesystem/operations.hpp>
#undef assert
#define assert xassert
#define yassert 1
using namespace boost::filesystem;          

#include "util/goodies.h"
#include "util/log.h"

