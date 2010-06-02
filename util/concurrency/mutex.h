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
        boost::thread_specific_ptr< set<mid> > us;
        map< mid, set<mid> > followers;
        boost::mutex &x;
        unsigned magic;
    public:
        void programEnding();
        MutexDebugger() : x( *(new boost::mutex()) ), magic(0x12345678) { }
        void entering(mid m) {
            if( magic != 0x12345678 ) return;
            set<mid> *preceeding = us.get();
            if( preceeding == 0 )
                us.reset( preceeding = new set<mid>() );
            
            bool failed = false;
            string err;
            {
                boost::mutex::scoped_lock lk(x);
                followers[m];
                for( set<mid>::iterator i = preceeding->begin(); i != preceeding->end(); i++ ) { 
                    followers[*i].insert(m);
                    if( m != *i && followers[m].count(*i) != 0 ){
                        failed = true;
                        stringstream ss;
                        ss << "mutex problem" <<
                            "\n  when locking " << m <<
                            "\n  " << *i << " was already locked and should not be.";
                        err = ss.str();
                        break;
                    }
                }
            }
            if( failed ) {
                cout << err << endl;
                assert( 0 );
            }
            preceeding->insert(m);
        }
        void leaving(mid m) { 
            if( magic != 0x12345678 ) return;
            us.get()->erase(m);
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
