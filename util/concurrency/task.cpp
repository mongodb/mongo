// @file task.cpp

/**
*    Copyright (C) 2008 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,b
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "pch.h"
#include "task.h"
#include "../goodies.h"
#include "../unittest.h"
#include "boost/thread/condition.hpp"

namespace mongo { 

    namespace task { 

        /*void foo() { 
            boost::mutex m;
            boost::mutex::scoped_lock lk(m);
            boost::condition cond;
            cond.wait(lk);
            cond.notify_one();
        }*/

        Task::Task() { 
            n = 0;
            repeat = 0;
            deleteSelf = true;
        }

        void Task::halt() { repeat = 0; }

        void Task::run() { 
            assert( n == 0 );
            while( 1 ) {
                n++;
                try { 
                    doWork();
                } 
                catch(...) { }
                if( repeat == 0 )
                    break;
                sleepmillis(repeat);
                if( inShutdown() )
                    break;
            }
        }

        void Task::begin() {
            go();
        }

        void fork(Task *t) { 
            t->begin();
        }

        void repeat(Task *t, unsigned millis) { 
            t->repeat = millis;
            t->begin();
        }

    }
}

#include "msg.h"

/* task::Server */

namespace mongo {
    namespace task {

        /* to get back a return value */
        struct Ret {
            Ret() : done(false) { }
            bool done;
            boost::mutex m;
            boost::condition c;
            const lam *msg;
            void f() {
                (*msg)();
                done = true;
                c.notify_one();
            }
        };

        void Server::call( const lam& msg ) {
            Ret r;
            r.msg = &msg;
            lam f = boost::bind(&Ret::f, &r);
            send(f);
            {
                boost::mutex::scoped_lock lk(r.m);
                while( !r.done )
                    r.c.wait(lk);
            }
        }

        void Server::send( lam msg ) { 
            {
                boost::mutex::scoped_lock lk(m);
                d.push_back(msg);
            }
            c.notify_one();
        }

        void Server::doWork() { 
            starting();
            rq = false;
            while( 1 ) { 
                lam f;
                try {
                    boost::mutex::scoped_lock lk(m);
                    while( d.empty() )
                        c.wait(lk);
                    f = d.front();
                    d.pop_front();
                }
                catch(...) { 
                    log() << "ERROR exception in Server:doWork?" << endl;
                }
                try {
                    f();
                    if( rq ) {
                        rq = false;
                        send(f);
                    }
                } catch(std::exception& e) { 
                    log() << "Server::doWork() exception " << e.what() << endl;
                } catch(...) {
                    log() << "Server::doWork() unknown exception!" << endl;
                }
            }
        }
        
    }
}
