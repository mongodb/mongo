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

namespace mongo {

#define NOMINMAX

#if defined(_WIN32)
    const bool debug=true;
#else
    const bool debug=false;
#endif

} // namespace mongo

#include <memory>
#include "stdlib.h"
#include "string.h"
#include "limits.h"

namespace mongo {

    void sayDbContext(const char *msg = 0);
    void dbexit(int returnCode, const char *whyMsg = "");
    void exit( int status );

    void printGitVersion();
    void printSysInfo();

    inline void * ourmalloc(size_t size) {
        void *x = malloc(size);
        if ( x == 0 ) dbexit(42, "malloc fails");
        return x;
    }

    inline void * ourrealloc(void *ptr, size_t size) {
        void *x = realloc(ptr, size);
        if ( x == 0 ) dbexit(43, "realloc fails");
        return x;
    }

#define malloc ourmalloc
#define realloc ourrealloc

} // namespace mongo

#include "targetver.h"

#include <string>
#include "time.h"

using namespace std;

namespace mongo {

    /* these are manipulated outside of mutexes, so be careful */
    struct Assertion {
        Assertion() {
            msg[0] = msg[127] = 0;
            context[0] = context[127] = 0;
            file = "";
            line = 0;
            when = 0;
        }
        char msg[128];
        char context[128];
        const char *file;
        unsigned line;
        time_t when;
        void set(const char *m, const char *ctxt, const char *f, unsigned l) {
            strncpy(msg, m, 127);
            strncpy(context, ctxt, 127);
            file = f;
            line = l;
            when = time(0);
        }
        string toString();
        bool isSet() {
            return when != 0;
        }
    };

    enum {
        AssertRegular = 0,
        AssertW = 1,
        AssertMsg = 2,
        AssertUser = 3
    };

    /* last assert of diff types: regular, wassert, msgassert, uassert: */
    extern Assertion lastAssert[4];

    class DBException : public exception {
    public:
        virtual const char* what() const throw() = 0;
        virtual string toString() const {
            return what();
        }
        operator string() const { return toString(); }
    };

    class AssertionException : public DBException {
    public:
        string msg;
        AssertionException() { }
        virtual ~AssertionException() throw() { }
        virtual bool severe() {
            return true;
        }
        virtual bool isUserAssertion() {
            return false;
        }
        virtual const char* what() const throw() { return msg.c_str(); }
    };

    /* UserExceptions are valid errors that a user can cause, like out of disk space or duplicate key */
    class UserException : public AssertionException {
    public:
        UserException(const char *_msg) {
            msg = _msg;
        }
        UserException(string _msg) {
            msg = _msg;
        }
        virtual bool severe() {
            return false;
        }
        virtual bool isUserAssertion() {
            return true;
        }
        virtual string toString() const {
            return "userassert:" + msg;
        }
    };

    class MsgAssertionException : public AssertionException {
    public:
        MsgAssertionException(const char *_msg) {
            msg = _msg;
        }
        virtual bool severe() {
            return false;
        }
        virtual string toString() const {
            return "massert:" + msg;
        }
    };

    void asserted(const char *msg, const char *file, unsigned line);
    void wasserted(const char *msg, const char *file, unsigned line);
    void uasserted(const char *msg);
    inline void uasserted(string msg) { uasserted(msg.c_str()); }
    void uassert_nothrow(const char *msg); // reported via lasterror, but don't throw exception
    void msgasserted(const char *msg);
    inline void msgasserted(string msg) { msgasserted(msg.c_str()); }

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
#if defined(_DEBUG)
#define dassert assert
#else
#define dassert(x) 
#endif

} // namespace mongo

#include <stdio.h>
#include <sstream>
#include <signal.h>

namespace mongo {

    typedef char _TCHAR;

} // namespace mongo

#include <iostream>
#include <fstream>
#include <map>
#include <vector>
#include <set>

namespace mongo {

//using namespace std;

#if !defined(_WIN32)
    typedef int HANDLE;
    inline void strcpy_s(char *dst, unsigned len, const char *src) {
        strcpy(dst, src);
    }
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

    void rawOut( const string &s );
    
} // namespace mongo

#include <vector>

namespace mongo {

// for debugging
    typedef struct _Ints {
        int i[100];
    } *Ints;
    typedef struct _Chars {
        char c[200];
    } *Chars;

    typedef char CHARS[400];

    typedef struct _OWS {
        int size;
        char type;
        char string[400];
    } *OWS;

    class Database;
    //extern Database *database;
    extern const char *curNs;

    /* for now, running on win32 means development not production --
       use this to log things just there.
    */
#if defined(_WIN32)
#define WIN if( 1 )
#else
#define WIN if( 0 )
#endif

#if defined(_DEBUG)
#define DEV if( 1 )
#else
#define DEV if( 0 )
#endif

#define DEBUGGING if( 0 )

    extern unsigned occasion;
    extern unsigned occasionR;
    extern unsigned once;

#define OCCASIONALLY if( ++occasion % 16 == 0 )
#define RARELY if( ++occasionR % 128 == 0 )
#define ONCE if( ++once == 1 )

#if defined(_WIN32)
#define strcasecmp _stricmp
    inline void our_debug_free(void *p) {
        unsigned *u = (unsigned *) p;
        u[0] = 0xEEEEEEEE;
        free(p);
    }
#define free our_debug_free
#endif

} // namespace mongo

#undef yassert
#include <boost/archive/iterators/base64_from_binary.hpp>
#include <boost/archive/iterators/binary_from_base64.hpp>
#include <boost/archive/iterators/transform_width.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/filesystem/convenience.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/program_options.hpp>
#include <boost/shared_ptr.hpp>
#define BOOST_SPIRIT_THREADSAFE
//#define BOOST_SPIRIT_DEBUG
#include <boost/spirit/core.hpp>
#include <boost/spirit/utility/loops.hpp>
#undef assert
#define assert xassert
#define yassert 1
using namespace boost::filesystem;

#include "util/goodies.h"
#include "util/log.h"
