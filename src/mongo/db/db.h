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
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#pragma once

#include "mongo/pch.h"

#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/database_holder.h"
#include "mongo/db/pdfile.h"
#include "mongo/util/net/message.h"

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
