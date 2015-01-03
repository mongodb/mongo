// @file task.cpp

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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kControl

#include "mongo/pch.h"

#include <boost/thread/condition.hpp>

#include "mongo/util/concurrency/task.h"

#include "mongo/db/repl/server.h"
#include "mongo/util/exit.h"
#include "mongo/util/log.h"
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
            lam f = stdx::bind(&Ret::f, &r);
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
                lam f = stdx::bind(abc, 3);
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
