// threadedtests.cpp - Tests for threaded code
//

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

#include "stdafx.h"
#include <boost/thread.hpp>
#include <boost/bind.hpp>

#include "dbtests.h"

namespace ThreadedTests {

    template <int nthreads_param=10>
    class ThreadedTest{
        public:
            virtual void setup() {} //optional
            virtual void subthread() = 0;
            virtual void validate() = 0;

            static const int nthreads = nthreads_param;

            void run(){
                setup();

                launch_subthreads(nthreads);

                validate();
            }

            virtual ~ThreadedTest() {}; // not necessary, but makes compilers happy

        private:
            void launch_subthreads(int remaining){
                if (!remaining) return;

                boost::thread athread(boost::bind(&ThreadedTest::subthread, this));

                launch_subthreads(remaining - 1);

                athread.join();
            }
    };

    // Tested with up to 30k threads
    class IsWrappingIntAtomic : public ThreadedTest<> {
        static const int iterations = 1000000;
        WrappingInt target;

        void subthread(){
            for(int i=0; i < iterations; i++){
                //target.x++; // verified to fail with this version
                target.atomicIncrement();
            }
        }
        void validate(){
            ASSERT_EQUALS(target.x , unsigned(nthreads * iterations));
        }
    };

    class All : public Suite {
    public:
        All() : Suite( "threading" ){
        }

        void setupTests(){
            add< IsWrappingIntAtomic >();
        }
    } myall;
}
