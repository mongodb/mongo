// assert_util.h

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

#include "../db/lasterror.h"

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
        std::string toString();
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

    class DBException : public std::exception {
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

#define BOOST_CHECK_EXCEPTION( expression ) \
	try { \
		expression; \
	} catch ( const std::exception &e ) { \
		problem() << "caught boost exception: " << e.what() << endl; \
		assert( false ); \
	} catch ( ... ) { \
		assert( false ); \
	}
