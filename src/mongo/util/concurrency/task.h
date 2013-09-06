// @file task.h

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

#pragma once

#include "mongo/util/background.h"

namespace mongo {

    namespace task {

        /** abstraction around threads.  simpler than BackgroundJob which is used behind the scenes.
            allocate the Task dynamically.  when the thread terminates, the Task object will delete itself.
        */
        class Task : private BackgroundJob {
        protected:
            virtual void setUp();  // Override to perform any do-once work for the task.
            virtual void doWork() = 0;                  // implement the task here.
            virtual string name() const = 0;            // name the thread
        public:
            Task();

            /** for a repeating task, stop after current invocation ends. can be called by other threads
                as long as the Task is still in scope.
                */
            void halt();
        private:
            unsigned n, repeat;
            friend void fork(Task* t);
            friend void repeat(Task* t, unsigned millis);
            virtual void run();
            //virtual void ending() { }
            void begin();
        };

        /** run once */
        void fork(Task *t);

        /** run doWork() over and over, with a pause between runs of millis */
        void repeat(Task *t, unsigned millis);

        /*** Example ***
        inline void sample() {
            class Sample : public Task {
            public:
                int result;
                virtual void doWork() { result = 1234; }
                Sample() : result(0) { }
            };
            shared_ptr<Sample> q( new Sample() );
            fork(q);
            cout << q->result << endl; // could print 1234 or 0.
        }
        */

    }

}
