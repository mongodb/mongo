// @file background_job_test.cpp

/**
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

#include "mongo/pch.h"

#include <boost/thread/thread.hpp>

#include "mongo/dbtests/dbtests.h"
#include "mongo/util/background.h"
#include "mongo/util/time_support.h"

namespace BackgroundJobTests {

    // a global variable that can be accessed independent of the IncTester object below
    // IncTester keeps it up-to-date
    int GLOBAL_val;

    class IncTester : public mongo::BackgroundJob {
    public:
        explicit IncTester( long long millis , bool selfDelete = false )
            : BackgroundJob(selfDelete), _val(0), _millis(millis) { GLOBAL_val = 0; }

        void waitAndInc( long long millis ) {
            if ( millis )
                mongo::sleepmillis( millis );
            ++_val;
            ++GLOBAL_val;
        }

        int getVal() { return _val; }

        /* --- BackgroundJob virtuals --- */

        string name() const { return "IncTester"; }

        void run() { waitAndInc( _millis ); }

    private:
        int _val;
        long long _millis;
    };


    class NormalCase {
    public:
        void run() {
            IncTester tester( 0 /* inc without wait */ );
            tester.go();
            ASSERT( tester.wait() );
            ASSERT_EQUALS( tester.getVal() , 1 );
        }
    };

    class TimeOutCase {
    public:
        void run() {
            IncTester tester( 2000 /* wait 2 sec before inc-ing */ );
            tester.go();
            ASSERT( ! tester.wait( 100 /* ms */ ) ); // should time out
            ASSERT_EQUALS( tester.getVal() , 0 );

            // if we wait longer than the IncTester, we should see the increment
            ASSERT( tester.wait( 4000 /* ms */ ) );  // should not time out
            ASSERT_EQUALS( tester.getVal() , 1 );
        }
    };

    class SelfDeletingCase {
    public:
        void run() {
            BackgroundJob* j = new IncTester( 0 /* inc without wait */ , true /* self delete */  );
            j->go();


            // the background thread should have continued running and this test should pass the
            // heap-checker as well
            mongo::sleepmillis( 1000 );
            ASSERT_EQUALS( GLOBAL_val, 1 );
        }
    };


    class BackgroundJobSuite : public Suite {
    public:
        BackgroundJobSuite() : Suite( "background_job" ) {}

        void setupTests() {
            // SelfDeletingCase uses a global value, so we run it first
            add< SelfDeletingCase >();
            add< NormalCase >();
            add< TimeOutCase >();
        }

    } backgroundJobSuite;

} // namespace BackgroundJobTests
