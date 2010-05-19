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
//#include "../../boosted/any.hpp"
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
/*            {
                cout << "TEME<EMEMEMP" << endl;
                boosted::any a;
                a = 3;
                a = string("AAA");
            }*/

            n = 0;
            repeat = 0;
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
            }
        }

        void Task::ending() { me.reset(); }

        void Task::begin(shared_ptr<Task> t) {
            me = t;
            go();
        }

        void fork(shared_ptr<Task> t) { 
            t->begin(t);
        }

        void repeat(shared_ptr<Task> t, unsigned millis) { 
            t->repeat = millis;
            t->begin(t);
        }

    }
}

#include "msg.h"

namespace mongo {
    namespace task {

        /* tests for messaging - see msg.h */

        /*
        class JustTesting : public Port<int> {
        protected:
            void got(const int& msg) { }
        public:
            virtual string name() { return "ASD"; }
            JustTesting() { }
        };    

        struct JTTest : public UnitTest {
            void run() { 
                foo();
                JustTesting *jt = new JustTesting();
                shared_ptr<Task> tp = jt->taskPtr();
                Task *t = tp.get();
                fork( tp );
                cout << "POKSDFFDSFDSFDSFDSFDS" << endl;

            } 
        } juttt;
        */
    }
}
