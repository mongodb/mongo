// locks.h

/*
 *    Copyright (C) 2010 10gen Inc.
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

//#include "../stdafx.h"

#pragma once

#if defined(_WIN32)

#if BOOST_VERSION >= 103500
#include <boost/thread/shared_mutex.hpp>
#undef assert
#define assert xassert
#else
#error need boost >= 1.35 for windows
#endif

#define BOOST_RWLOCK

#else

#include <pthread.h>

#endif

namespace mongo {

#ifdef BOOST_RWLOCK
    class RWLock {
        boost::shared_mutex _m;

    public:
        void lock(){
            _m.lock();
        }
        void unlock(){
            _m.unlock();
        }
        
        void lock_shared(){
            _m.lock_shared();
        }
        
        void unlock_shared(){
            _m.unlock_shared();
        }

        bool lock_shared_try( int millis ){
            boost::system_time until = get_system_time();
            until += boost::posix_time::milliseconds(millis);
            return _m.timed_lock_shared( until );
        }

        bool lock_try( int millis ){
            boost::system_time until = get_system_time();
            until += boost::posix_time::milliseconds(millis);
            return _m.timed_lock( until );
        }


    };
#else
    class RWLock {
        pthread_rwlock_t _lock;

        inline void check( int x ){
            if( x == 0 )
                return;
            log() << "pthread rwlock failed: " << x << endl;
            assert( x == 0 );
        }

    public:
        RWLock(){
            check( pthread_rwlock_init( &_lock , 0 ) );
        }
        
        ~RWLock(){
            if ( ! __destroyingStatics ){
                check( pthread_rwlock_destroy( &_lock ) );
            }
        }

        void lock(){
            check( pthread_rwlock_wrlock( &_lock ) );
        }
        void unlock(){
            check( pthread_rwlock_unlock( &_lock ) );
        }
        
        void lock_shared(){
            check( pthread_rwlock_rdlock( &_lock ) );
        }
        
        void unlock_shared(){
            check( pthread_rwlock_unlock( &_lock ) );
        }
        
        bool lock_shared_try( int millis ){
            return _try( millis , false );
        }

        bool lock_try( int millis ){
            return _try( millis , true );
        }

        bool _try( int millis , bool write ){
            while ( true ) {
                int x = write ? 
                    pthread_rwlock_trywrlock( &_lock ) : 
                    pthread_rwlock_tryrdlock( &_lock );
                
                if ( x <= 0 )
                    return true;
                
                if ( millis-- <= 0 )
                    return false;
                
                if ( x == EBUSY ){
                    sleepmillis(1);
                    continue;
                }
                check(x);
            } 
            
            return false;
        }

    };
    

#endif

    struct rwlock {
        rwlock( RWLock& lock , bool write , bool alreadyHaveLock = false )
            : _lock( lock ) , _write( write ){

            if ( ! alreadyHaveLock ){
                if ( _write )
                    _lock.lock();
                else
                    _lock.lock_shared();
            }
        }

        ~rwlock(){
            if ( _write )
                _lock.unlock();
            else
                _lock.unlock_shared();
        }
        
        RWLock& _lock;
        bool _write;
    };
}
