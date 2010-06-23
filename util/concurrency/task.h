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
*/

#pragma once

#include "../background.h"

namespace mongo { 

    namespace task { 

        /** abstraction around threads.  simpler than BackgroundJob which is used behind the scenes.
            a shared_ptr is kept to the task - both by the task while running and the caller.  that way 
            the task object gets cleaned up once the last reference goes away. 
        */
        class Task : private BackgroundJob {
        protected:
            virtual void doWork() = 0;                  // implement the task here.
            virtual string name() = 0;                  // name the threada
        public:
            Task();

            /** for a repeating task, stop after current invocation ends. */
            void halt();
        private:
            shared_ptr<Task> me;
            unsigned n, repeat;
            friend void fork(shared_ptr<Task> t);
            friend void repeat(shared_ptr<Task> t, unsigned millis);
            virtual void run();
            virtual void ending();
            void begin(shared_ptr<Task>);
        };

        /** run once */
        void fork(shared_ptr<Task> t);

        /** run doWork() over and over, with a pause between runs of millis */
        void repeat(shared_ptr<Task> t, unsigned millis);

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
