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

#include "../pch.h"
#include "../util/net/message.h"
#include "mongomutex.h"
#include "pdfile.h"
#include "curop.h"
#include "client.h"
#include "databaseholder.h"

namespace mongo {

    // todo: relocked is being called when there was no unlock below. 
    //       that is weird.

    struct dbtemprelease {
        Client::Context * _context;
        scoped_ptr<Lock::TempRelease> tr;
        dbtemprelease() {
            const Client& c = cc();
            _context = c.getContext();
            verify( Lock::isLocked() );
            if( Lock::nested() ) {
                Lock::nested();
                massert(10298 , "can't temprelease nested lock", false);
            }
            if ( _context ) {
                _context->unlocked();
            }
            tr.reset(new Lock::TempRelease);
            verify( c.curop() );
            c.curop()->yielded();
        }
        ~dbtemprelease() {
            tr.reset();
            if ( _context ) 
                _context->relocked();
        }
    };

    /** must be write locked
        no verify(and no release) if nested write lock 
        a lot like dbtempreleasecond, eliminate?
    */
    struct dbtempreleasewritelock {
        Client::Context * _context;
        int _locktype;
        scoped_ptr<Lock::TempRelease> tr;
        dbtempreleasewritelock() {
            const Client& c = cc();
            _context = c.getContext();
            verify( Lock::isW() );
            if( Lock::nested() )
                return;
            if ( _context ) 
                _context->unlocked();
            tr.reset(new Lock::TempRelease);
            verify( c.curop() );
            c.curop()->yielded();            
        }
        ~dbtempreleasewritelock() {
            tr.reset();
            if ( _context ) 
                _context->relocked();
        }
    };

    /**
       only does a temp release if we're not nested and have a lock
     */
    class dbtempreleasecond : boost::noncopyable {
        dbtemprelease * real;
    public:
        dbtempreleasecond() {
            real = 0;
            if( Lock::isLocked() ) {
                // if nested don't temprelease, and we don't complain either for this class
                if( !Lock::nested() ) {
                    real = new dbtemprelease();
                }
            }
        }
        ~dbtempreleasecond() {
            if ( real ) {
                delete real;
                real = 0;
            }
        }
        bool unlocked() const { return real != 0; }
    };

} // namespace mongo
