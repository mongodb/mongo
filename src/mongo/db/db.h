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

    struct dbtemprelease {
        Client::Context * _context;
        int _locktype;

        dbtemprelease() {
            const Client& c = cc();
            _context = c.getContext();
            _locktype = d.dbMutex.getState();
            assert( _locktype );

            if ( _locktype > 0 ) {
                massert( 10298 , "can't temprelease nested write lock", _locktype == 1);
                if ( _context ) _context->unlocked();
                d.dbMutex.unlock();
            }
            else {
                massert( 10299 , "can't temprelease nested read lock", _locktype == -1);
                if ( _context ) _context->unlocked();
                d.dbMutex.unlock_shared();
            }
            
            verify( 14814 , c.curop() );
            c.curop()->yielded();
            
        }
        ~dbtemprelease() {
            if ( _locktype > 0 )
                d.dbMutex.lock();
            else
                d.dbMutex.lock_shared();

            if ( _context ) _context->relocked();
        }
    };

    /** must be write locked
        no assert (and no release) if nested write lock 
        a lot like dbtempreleasecond but no malloc so should be a tiny bit faster
    */
    struct dbtempreleasewritelock {
        Client::Context * _context;
        int _locktype;
        dbtempreleasewritelock() {
            const Client& c = cc();
            _context = c.getContext();
            _locktype = d.dbMutex.getState();
            assert( _locktype >= 1 );
            if( _locktype > 1 ) 
                return; // nested
            if ( _context ) 
                _context->unlocked();
            d.dbMutex.unlock();
            verify( 14845 , c.curop() );
            c.curop()->yielded();            
        }
        ~dbtempreleasewritelock() {
            if ( _locktype == 1 )
                d.dbMutex.lock();
            if ( _context ) 
                _context->relocked();
        }
    };

    /**
       only does a temp release if we're not nested and have a lock
     */
    struct dbtempreleasecond {
        dbtemprelease * real;
        int locktype;

        dbtempreleasecond() {
            real = 0;
            locktype = d.dbMutex.getState();
            if ( locktype == 1 || locktype == -1 )
                real = new dbtemprelease();
        }

        ~dbtempreleasecond() {
            if ( real ) {
                delete real;
                real = 0;
            }
        }

        bool unlocked() {
            return real != 0;
        }
    };

} // namespace mongo
