// stdafx.cpp : source file that includes just the standard includes
// db.pch will be the pre-compiled header
// stdafx.obj will contain the pre-compiled type information

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

#include "stdafx.h"

namespace mongo {

// TODO: reference any additional headers you need in STDAFX.H
// and not in this file

    Assertion lastAssert[4];

#undef assert

#undef yassert

} // namespace mongo

#include "assert.h"
#include "db/lasterror.h"

namespace mongo {

    string getDbContext();

    /* "warning" assert -- safe to continue, so we don't throw exception. */
    void wasserted(const char *msg, const char *file, unsigned line) {
        problem() << "Assertion failure " << msg << ' ' << file << ' ' << dec << line << endl;
        sayDbContext();
        raiseError(msg && *msg ? msg : "wassertion failure");
        lastAssert[1].set(msg, getDbContext().c_str(), file, line);
    }

    void asserted(const char *msg, const char *file, unsigned line) {
        problem() << "Assertion failure " << msg << ' ' << file << ' ' << dec << line << endl;
        sayDbContext();
        raiseError(msg && *msg ? msg : "assertion failure");
        lastAssert[0].set(msg, getDbContext().c_str(), file, line);
        throw AssertionException();
    }

    void uassert_nothrow(const char *msg) {
        lastAssert[3].set(msg, getDbContext().c_str(), "", 0);
        raiseError(msg);
    }

    int uacount = 0;
    void uasserted(const char *msg) {
        if ( ++uacount < 100 )
            log() << "User Exception " << msg << endl;
        else
            RARELY log() << "User Exception " << msg << endl;
        lastAssert[3].set(msg, getDbContext().c_str(), "", 0);
        raiseError(msg);
        throw UserException(msg);
    }

    void msgasserted(const char *msg) {
        log() << "Assertion: " << msg << '\n';
        lastAssert[2].set(msg, getDbContext().c_str(), "", 0);
        raiseError(msg && *msg ? msg : "massert failure");
        throw MsgAssertionException(msg);
    }

    string Assertion::toString() {
        if ( !isSet() )
            return "";

        stringstream ss;
        ss << msg << '\n';
        if ( *context )
            ss << context << '\n';
        if ( *file )
            ss << file << ' ' << line << '\n';
        return ss.str();
    }

    /* this is a good place to set a breakpoint when debugging, as lots of warning things
       (assert, wassert) call it.
    */
    void sayDbContext(const char *errmsg) {
        if ( errmsg ) {
            problem() << errmsg << endl;
        }
        printStackTrace();
    }
    
    void exit( int status ){
        dbexit( status );
    }

    void rawOut( const string &s ) {
        if( s.empty() ) return;
        char now[64];
        time_t_to_String(time(0), now);
        now[20] = 0;        
#if defined(_WIN32)
        (std::cout << now << " " << s).flush();
#else
        write( STDOUT_FILENO, now, 20 );
        write( STDOUT_FILENO, " ", 1 );
        write( STDOUT_FILENO, s.c_str(), s.length() );
        fsync( STDOUT_FILENO );        
#endif
    }

#ifndef _SCONS
    // only works in scons
    void printGitVersion(){}
    void printSysInfo(){}
#endif

} // namespace mongo
