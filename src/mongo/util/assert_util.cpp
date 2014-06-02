// assert_util.cpp

/*    Copyright 2009 10gen Inc.
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
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/pch.h"

#include "mongo/util/assert_util.h"

using namespace std;

#ifndef _WIN32
#include <cxxabi.h>
#include <sys/file.h>
#endif

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/lasterror.h"
#include "mongo/util/stacktrace.h"

namespace mongo {

    AssertionCount assertionCount;

    AssertionCount::AssertionCount()
        : regular(0),warning(0),msg(0),user(0),rollovers(0) {
    }

    void AssertionCount::rollover() {
        rollovers++;
        regular = 0;
        warning = 0;
        msg = 0;
        user = 0;
    }

    void AssertionCount::condrollover( int newvalue ) {
        static const int rolloverPoint = ( 1 << 30 );
        if ( newvalue >= rolloverPoint )
            rollover();
    }

    bool DBException::traceExceptions = false;

    string DBException::toString() const {
        stringstream ss;
        ss << getCode() << " " << what();
        return ss.str();
    }

    void DBException::traceIfNeeded( const DBException& e ) {
        if( traceExceptions && ! inShutdown() ){
            warning() << "DBException thrown" << causedBy( e ) << endl;
            printStackTrace();
        }
    }

    ErrorCodes::Error DBException::convertExceptionCode(int exCode) {
        if (exCode == 0) return ErrorCodes::UnknownError;
        return static_cast<ErrorCodes::Error>(exCode);
    }

    void ExceptionInfo::append( BSONObjBuilder& b , const char * m , const char * c ) const {
        if ( msg.empty() )
            b.append( m , "unknown assertion" );
        else
            b.append( m , msg );

        if ( code )
            b.append( c , code );
    }

    /* "warning" assert -- safe to continue, so we don't throw exception. */
    NOINLINE_DECL void wasserted(const char *msg, const char *file, unsigned line) {
        static bool rateLimited;
        static time_t lastWhen;
        static unsigned lastLine;
        if( lastLine == line && time(0)-lastWhen < 5 ) { 
            if( !rateLimited ) { 
                rateLimited = true;
                log() << "rate limiting wassert" << endl;
            }
            return;
        }
        lastWhen = time(0);
        lastLine = line;

        log() << "warning assertion failure " << msg << ' ' << file << ' ' << dec << line << endl;
        logContext();
        setLastError(0,msg && *msg ? msg : "wassertion failure");
        assertionCount.condrollover( ++assertionCount.warning );
#if defined(_DEBUG) || defined(_DURABLEDEFAULTON) || defined(_DURABLEDEFAULTOFF)
        // this is so we notice in buildbot
        log() << "\n\n***aborting after wassert() failure in a debug/test build\n\n" << endl;
        abort();
#endif
    }

    NOINLINE_DECL void verifyFailed(const char *msg, const char *file, unsigned line) {
        assertionCount.condrollover( ++assertionCount.regular );
        log() << "Assertion failure " << msg << ' ' << file << ' ' << dec << line << endl;
        logContext();
        setLastError(0,msg && *msg ? msg : "assertion failure");
        stringstream temp;
        temp << "assertion " << file << ":" << line;
        AssertionException e(temp.str(),0);
        breakpoint();
#if defined(_DEBUG) || defined(_DURABLEDEFAULTON) || defined(_DURABLEDEFAULTOFF)
        // this is so we notice in buildbot
        log() << "\n\n***aborting after verify() failure as this is a debug/test build\n\n" << endl;
        abort();
#endif
        throw e;
    }

    NOINLINE_DECL void invariantFailed(const char *msg, const char *file, unsigned line) {
        log() << "Invariant failure " << msg << ' ' << file << ' ' << dec << line << endl;
        logContext();
        breakpoint();
        log() << "\n\n***aborting after invariant() failure\n\n" << endl;
        abort();
    }

    NOINLINE_DECL void fassertFailed( int msgid ) {
        log() << "Fatal Assertion " << msgid << endl;
        logContext();
        breakpoint();
        log() << "\n\n***aborting after fassert() failure\n\n" << endl;
        abort();
    }

    NOINLINE_DECL void fassertFailedNoTrace( int msgid ) {
        log() << "Fatal Assertion " << msgid << endl;
        breakpoint();
        log() << "\n\n***aborting after fassert() failure\n\n" << endl;
        ::_exit(EXIT_ABRUPT); // bypass our handler for SIGABRT, which prints a stack trace.
    }

    MONGO_COMPILER_NORETURN void fassertFailedWithStatus(int msgid, const Status& status) {
        log() << "Fatal assertion " <<  msgid << " " << status;
        logContext();
        breakpoint();
        log() << "\n\n***aborting after fassert() failure\n\n" << endl;
        abort();
    }

    void uasserted(int msgid , const string &msg) {
        uasserted(msgid, msg.c_str());
    }

    void UserException::appendPrefix( stringstream& ss ) const { ss << "userassert:"; }
    void MsgAssertionException::appendPrefix( stringstream& ss ) const { ss << "massert:"; }

    NOINLINE_DECL void uasserted(int msgid, const char *msg) {
        assertionCount.condrollover( ++assertionCount.user );
        LOG(1) << "User Assertion: " << msgid << ":" << msg << endl;
        setLastError(msgid,msg);
        throw UserException(msgid, msg);
    }

    void msgasserted(int msgid, const string &msg) {
        msgasserted(msgid, msg.c_str());
    }

    NOINLINE_DECL void msgasserted(int msgid, const char *msg) {
        assertionCount.condrollover( ++assertionCount.warning );
        log() << "Assertion: " << msgid << ":" << msg << endl;
        setLastError(msgid,msg && *msg ? msg : "massert failure");
        //breakpoint();
        logContext();
        throw MsgAssertionException(msgid, msg);
    }

    NOINLINE_DECL void msgassertedNoTrace(int msgid, const char *msg) {
        assertionCount.condrollover( ++assertionCount.warning );
        log() << "Assertion: " << msgid << ":" << msg << endl;
        setLastError(msgid,msg && *msg ? msg : "massert failure");
        throw MsgAssertionException(msgid, msg);
    }

    void msgassertedNoTrace(int msgid, const std::string& msg) {
        msgassertedNoTrace(msgid, msg.c_str());
    }

    std::string causedBy( const char* e ) {
        return std::string(" :: caused by :: ") + e;
    }

    std::string causedBy( const DBException& e ){
        return causedBy( e.toString() );
    }

    std::string causedBy( const std::exception& e ) {
        return causedBy( e.what() );
    }

    std::string causedBy( const std::string& e ){
        return causedBy( e.c_str() );
    }

    std::string causedBy( const std::string* e ) {
        return (e && *e != "") ? causedBy(*e) : "";
    }

    std::string causedBy( const Status& e ){
        return causedBy( e.reason() );
    }

    NOINLINE_DECL void streamNotGood( int code , const std::string& msg , std::ios& myios ) {
        stringstream ss;
        // errno might not work on all systems for streams
        // if it doesn't for a system should deal with here
        ss << msg << " stream invalid: " << errnoWithDescription();
        throw UserException( code , ss.str() );
    }

    string errnoWithPrefix( const char * prefix ) {
        stringstream ss;
        if ( prefix )
            ss << prefix << ": ";
        ss << errnoWithDescription();
        return ss.str();
    }

    string demangleName( const type_info& typeinfo ) {
#ifdef _WIN32
        return typeinfo.name();
#else
        int status;

        char * niceName = abi::__cxa_demangle(typeinfo.name(), 0, 0, &status);
        if ( ! niceName )
            return typeinfo.name();

        string s = niceName;
        free(niceName);
        return s;
#endif
    }

    string ExceptionInfo::toString() const {
        stringstream ss; ss << "exception: " << code << " " << msg; return ss.str(); 
    }

    NOINLINE_DECL ErrorMsg::ErrorMsg(const char *msg, char ch) {
        int l = strlen(msg);
        verify( l < 128);
        memcpy(buf, msg, l);
        char *p = buf + l;
        p[0] = ch;
        p[1] = 0;
    }

    NOINLINE_DECL ErrorMsg::ErrorMsg(const char *msg, unsigned val) {
        int l = strlen(msg);
        verify( l < 128);
        memcpy(buf, msg, l);
        char *p = buf + l;
        sprintf(p, "%u", val);
    }

}
