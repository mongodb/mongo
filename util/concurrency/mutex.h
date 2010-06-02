// @file mutex.h

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

#include <map>
#include <set>

namespace mongo { 

    extern bool __destroyingStatics;

    class mutex;
    class MutexDebugger { 
        typedef const char * mid; // mid = mutex ID
        typedef map<mid,int> Preceeding;
        map< mid, int > maxNest;
        boost::thread_specific_ptr< Preceeding > us;
        map< mid, set<mid> > followers;
        boost::mutex &x;
        unsigned magic;
    public:
        void programEnding();
        MutexDebugger() : x( *(new boost::mutex()) ), magic(0x12345678) { }
        void entering(mid m) {
            if( magic != 0x12345678 ) return;
            Preceeding *_preceeding = us.get();
            if( _preceeding == 0 )
                us.reset( _preceeding = new Preceeding() );
            Preceeding &preceeding = *_preceeding;

            preceeding[m]++;
            if( preceeding[m] > 1 ) { 
                // recursive re-locking.
                if( preceeding[m] > maxNest[m] )
                    maxNest[m] = preceeding[m];
                return;
            }

            bool failed = false;
            string err;
            {
                boost::mutex::scoped_lock lk(x);
                followers[m];
                for( Preceeding::iterator i = preceeding.begin(); i != preceeding.end(); i++ ) { 
                    if( m != i->first && i->second > 0 ) {
                        followers[i->first].insert(m);
                        if( followers[m].count(i->first) != 0 ){
                            failed = true;
                            stringstream ss;
                            ss << "mutex problem" <<
                                "\n  when locking " << m <<
                                "\n  " << i->first << " was already locked and should not be.\n";
                            ss << "locked before " << m << " in this thread:\n";
                            for( Preceeding::iterator i = preceeding.begin(); i != preceeding.end(); i++ ) { 
                                if( i->first != m && i->second > 0 )
                                    ss << "  " << i->first << '\n';
                            }
                            err = ss.str();
                            break;
                        }
                    }
                }
            }
            if( failed ) {
                cout << err << endl;
                assert( 0 );
            }
        }
        void leaving(mid m) { 
            if( magic != 0x12345678 ) return;
            Preceeding& preceeding = *us.get();
            preceeding[m]--;
            if( preceeding[m] < 0 ) {
                cout << "ERROR: lock count for " << m << " is " << preceeding[m] << endl;
                assert( preceeding[m] >= 0 );
            }
        }
    };
    extern MutexDebugger mutexDebugger;
    
    // If you create a local static instance of this class, that instance will be destroyed
    // before all global static objects are destroyed, so __destroyingStatics will be set
    // to true before the global static variables are destroyed.
    class StaticObserver : boost::noncopyable {
    public:
        ~StaticObserver() { __destroyingStatics = true; }
    };

    // On pthread systems, it is an error to destroy a mutex while held.  Static global
    // mutexes may be held upon shutdown in our implementation, and this way we avoid
    // destroying them.
    class mutex : boost::noncopyable {
    public:
#if defined(_DEBUG)
        const char *_name;
#endif

#if defined(_DEBUG)
        mutex(const char *name) 
           : _name(name) 
#else
        mutex(const char *) 
#endif
        { 
            _m = new boost::mutex(); 
        }
        ~mutex() {
            if( !__destroyingStatics ) {
                delete _m;
            }
        }
        class scoped_lock : boost::noncopyable {
#if defined(_DEBUG)
            mongo::mutex *mut;
#endif
        public:
            scoped_lock( mongo::mutex &m ) : _l( m.boost() ) {
#if defined(_DEBUG)
                mut = &m;
                mutexDebugger.entering(mut->_name);
#endif
            }
            ~scoped_lock() { 
#if defined(_DEBUG)
                mutexDebugger.leaving(mut->_name);
#endif
            }
            boost::mutex::scoped_lock &boost() { return _l; }
        private:
            boost::mutex::scoped_lock _l;
        };
    private:
        boost::mutex &boost() { return *_m; }
        boost::mutex *_m;
    };
    
    typedef mutex::scoped_lock scoped_lock;
    typedef boost::recursive_mutex::scoped_lock recursive_scoped_lock;

}
