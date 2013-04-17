// @file task.cpp

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

#include "mongo/pch.h"

#include <boost/thread/condition.hpp>

#include "mongo/util/concurrency/task.h"

#include "mongo/util/concurrency/msg.h"
#include "mongo/util/goodies.h"
#include "mongo/util/startup_test.h"
#include "mongo/util/time_support.h"

namespace mongo {

    namespace task {

        /*void foo() {
            boost::mutex m;
            boost::mutex::scoped_lock lk(m);
            boost::condition cond;
            cond.wait(lk);
            cond.notify_one();
        }*/

        Task::Task()
            : BackgroundJob( true /* deleteSelf */ ) {
            n = 0;
            repeat = 0;
        }

        void Task::halt() { repeat = 0; }

        void Task::setUp() {}

        void Task::run() {
            verify( n == 0 );

            setUp();

            while( 1 ) {
                n++;
                try {
                    doWork();
                }
                catch(...) { }
                sleepmillis(repeat);
                if( inShutdown() )
                    break;
                if( repeat == 0 )
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



/* task::Server */

namespace mongo {
    namespace task {

        /* to get back a return value */
        struct Ret {
            Ret() : done(false),m("Ret") { }
            bool done;
            mongo::mutex m;
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
                scoped_lock lk(r.m);
                while( !r.done )
                    r.c.wait(lk.boost());
            }
        }

        void Server::send( lam msg ) {
            {
                scoped_lock lk(m);
                d.push_back(msg);
                wassert( d.size() < 1024 );
            }
            c.notify_one();
        }

        void Server::doWork() {
            starting();
            while( 1 ) {
                lam f;
                try {
                    scoped_lock lk(m);
                    while( d.empty() )
                        c.wait(lk.boost());
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
                        {
                            scoped_lock lk(m);
                            d.push_back(f);
                        }
                    }
                }
                catch(std::exception& e) {
                    log() << "Server::doWork task:" << name() << " exception:" << e.what() << endl;
                }
                catch(const char *p) {
                    log() << "Server::doWork task:" << name() << " unknown c exception:" <<
                          ((p&&strlen(p)<800)?p:"?") << endl;
                }
                catch(...) {
                    log() << "Server::doWork unknown exception task:" << name() << endl;
                }
            }
        }

        static Server *s;
        static void abc(int i) {
            cout << "Hello " << i << endl;
            s->requeue();
        }
        class TaskUnitTest : public mongo::StartupTest {
        public:
            virtual void run() {
                lam f = boost::bind(abc, 3);
                //f();

                s = new Server("unittest");
                fork(s);
                s->send(f);

                sleepsecs(30);
                cout <<" done" << endl;

            }
        }; // not running. taskunittest;

    }
}
