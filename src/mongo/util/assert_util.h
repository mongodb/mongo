// assert_util.h

/*    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#pragma once

#include <iostream>
#include <typeinfo>

#include "mongo/bson/inline_decls.h"
#include "mongo/platform/compiler.h"

namespace mongo {

    enum CommonErrorCodes {
        DatabaseDifferCaseCode = 13297 ,
        SendStaleConfigCode = 13388 ,
        RecvStaleConfigCode = 9996
    };

    class AssertionCount {
    public:
        AssertionCount();
        void rollover();
        void condrollover( int newValue );

        int regular;
        int warning;
        int msg;
        int user;
        int rollovers;
    };

    extern AssertionCount assertionCount;

    class BSONObjBuilder;

    struct ExceptionInfo {
        ExceptionInfo() : msg(""),code(-1) {}
        ExceptionInfo( const char * m , int c )
            : msg( m ) , code( c ) {
        }
        ExceptionInfo( const std::string& m , int c )
            : msg( m ) , code( c ) {
        }
        void append( BSONObjBuilder& b , const char * m = "$err" , const char * c = "code" ) const ;
        std::string toString() const;
        bool empty() const { return msg.empty(); }        
        void reset(){ msg = ""; code=-1; }
        std::string msg;
        int code;
    };

    /** helper class that builds error strings.  lighter weight than a StringBuilder, albeit less flexible.
        NOINLINE_DECL used in the constructor implementations as we are assuming this is a cold code path when used.

        example: 
          throw UserException(123, ErrorMsg("blah", num_val));
    */
    class ErrorMsg { 
    public:
        ErrorMsg(const char *msg, char ch);
        ErrorMsg(const char *msg, unsigned val);
        operator std::string() const { return buf; }
    private:
        char buf[256];
    };

    class DBException;
    std::string causedBy( const DBException& e );
    std::string causedBy( const std::string& e );
    bool inShutdown();

    /** Most mongo exceptions inherit from this; this is commonly caught in most threads */
    class DBException : public std::exception {
    public:
        DBException( const ExceptionInfo& ei ) : _ei(ei) { traceIfNeeded(*this); }
        DBException( const char * msg , int code ) : _ei(msg,code) { traceIfNeeded(*this); }
        DBException( const std::string& msg , int code ) : _ei(msg,code) { traceIfNeeded(*this); }
        virtual ~DBException() throw() { }

        virtual const char* what() const throw() { return _ei.msg.c_str(); }
        virtual int getCode() const { return _ei.code; }

        virtual void appendPrefix( std::stringstream& ss ) const { }
        virtual void addContext( const std::string& str ) {
            _ei.msg = str + causedBy( _ei.msg );
        }

        virtual std::string toString() const;

        const ExceptionInfo& getInfo() const { return _ei; }
    private:
        static void traceIfNeeded( const DBException& e );
    public:
        static bool traceExceptions;

    protected:
        ExceptionInfo _ei;
    };

    class AssertionException : public DBException {
    public:

        AssertionException( const ExceptionInfo& ei ) : DBException(ei) {}
        AssertionException( const char * msg , int code ) : DBException(msg,code) {}
        AssertionException( const std::string& msg , int code ) : DBException(msg,code) {}

        virtual ~AssertionException() throw() { }

        virtual bool severe() { return true; }
        virtual bool isUserAssertion() { return false; }

        /* true if an interrupted exception - see KillCurrentOp */
        bool interrupted() {
            return _ei.code == 11600 || _ei.code == 11601;
        }
    };

    /* UserExceptions are valid errors that a user can cause, like out of disk space or duplicate key */
    class UserException : public AssertionException {
    public:
        UserException(int c , const std::string& m) : AssertionException( m , c ) {}
        virtual bool severe() { return false; }
        virtual bool isUserAssertion() { return true; }
        virtual void appendPrefix( std::stringstream& ss ) const;
    };

    class MsgAssertionException : public AssertionException {
    public:
        MsgAssertionException( const ExceptionInfo& ei ) : AssertionException( ei ) {}
        MsgAssertionException(int c, const std::string& m) : AssertionException( m , c ) {}
        virtual bool severe() { return false; }
        virtual void appendPrefix( std::stringstream& ss ) const;
    };

    MONGO_COMPILER_NORETURN void verifyFailed(const char *msg, const char *file, unsigned line);
    void wasserted(const char *msg, const char *file, unsigned line);
    MONGO_COMPILER_NORETURN void fassertFailed( int msgid );
    
    /** a "user assertion".  throws UserAssertion.  logs.  typically used for errors that a user
        could cause, such as duplicate key, disk full, etc.
    */
    MONGO_COMPILER_NORETURN void uasserted(int msgid, const char *msg);
    MONGO_COMPILER_NORETURN void uasserted(int msgid , const std::string &msg);

    /** msgassert and massert are for errors that are internal but have a well defined error text std::string.
        a stack trace is logged.
    */
    MONGO_COMPILER_NORETURN void msgassertedNoTrace(int msgid, const char *msg);
    inline void msgassertedNoTrace(int msgid, const std::string& msg) { msgassertedNoTrace( msgid , msg.c_str() ); }
    MONGO_COMPILER_NORETURN void msgasserted(int msgid, const char *msg);
    MONGO_COMPILER_NORETURN void msgasserted(int msgid, const std::string &msg);

    /* convert various types of exceptions to strings */
    inline std::string causedBy( const char* e ){ return (std::string)" :: caused by :: " + e; }
    inline std::string causedBy( const DBException& e ){ return causedBy( e.toString().c_str() ); }
    inline std::string causedBy( const std::exception& e ){ return causedBy( e.what() ); }
    inline std::string causedBy( const std::string& e ){ return causedBy( e.c_str() ); }

    /** abends on condition failure */
    inline void fassert( int msgid , bool testOK ) { if ( ! testOK ) fassertFailed( msgid ); }


    /* "user assert".  if asserts, user did something wrong, not our code */
#define MONGO_uassert(msgid, msg, expr) (void)( MONGO_likely(!!(expr)) || (mongo::uasserted(msgid, msg), 0) )

    /* warning only - keeps going */
#define MONGO_wassert(_Expression) (void)( MONGO_likely(!!(_Expression)) || (mongo::wasserted(#_Expression, __FILE__, __LINE__), 0) )

    /* display a message, no context, and throw assertionexception

       easy way to throw an exception and log something without our stack trace
       display happening.
    */
#define MONGO_massert(msgid, msg, expr) (void)( MONGO_likely(!!(expr)) || (mongo::msgasserted(msgid, msg), 0) )
    /* same as massert except no msgid */
#define MONGO_verify(_Expression) (void)( MONGO_likely(!!(_Expression)) || (mongo::verifyFailed(#_Expression, __FILE__, __LINE__), 0) )

    /* dassert is 'debug assert' -- might want to turn off for production as these
       could be slow.
    */
#if defined(_DEBUG)
# define MONGO_dassert verify
#else
# define MONGO_dassert(x)
#endif


#ifdef MONGO_EXPOSE_MACROS
# define dassert MONGO_dassert
# define verify MONGO_verify
# define uassert MONGO_uassert
# define wassert MONGO_wassert
# define massert MONGO_massert
#endif

    // some special ids that we want to duplicate

    // > 10000 asserts
    // < 10000 UserException

    enum { ASSERT_ID_DUPKEY = 11000 };

    /* throws a uassertion with an appropriate msg */
    MONGO_COMPILER_NORETURN void streamNotGood( int code , std::string msg , std::ios& myios );

    inline void assertStreamGood(unsigned msgid, std::string msg, std::ios& myios) {
        if( !myios.good() ) streamNotGood(msgid, msg, myios);
    }

    std::string demangleName( const std::type_info& typeinfo );

} // namespace mongo

#define MONGO_ASSERT_ON_EXCEPTION( expression ) \
    try { \
        expression; \
    } catch ( const std::exception &e ) { \
        stringstream ss; \
        ss << "caught exception: " << e.what() << ' ' << __FILE__ << ' ' << __LINE__; \
        msgasserted( 13294 , ss.str() ); \
    } catch ( ... ) { \
        massert( 10437 ,  "unknown exception" , false ); \
    }

#define MONGO_ASSERT_ON_EXCEPTION_WITH_MSG( expression, msg ) \
    try { \
        expression; \
    } catch ( const std::exception &e ) { \
        stringstream ss; \
        ss << msg << " caught exception exception: " << e.what();   \
        msgasserted( 14043 , ss.str() );        \
    } catch ( ... ) { \
        msgasserted( 14044 , std::string("unknown exception") + msg );   \
    }

#define DESTRUCTOR_GUARD MONGO_DESTRUCTOR_GUARD
#define MONGO_DESTRUCTOR_GUARD( expression ) \
    try { \
        expression; \
    } catch ( const std::exception &e ) { \
        problem() << "caught exception (" << e.what() << ") in destructor (" << __FUNCTION__ << ")" << endl; \
    } catch ( ... ) { \
        problem() << "caught unknown exception in destructor (" << __FUNCTION__ << ")" << endl; \
    }

