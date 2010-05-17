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

        class Task : protected BackgroundJob { 
            virtual void run();
        protected:
            virtual void go() = 0;
        public:
            virtual ~Task();
        };

        void fork(const shared_ptr<Task>& t);

        /*** Example ***
        inline void sample() { 
            class Sample : public Task { 
            public:
                int result;
                virtual void go() { result = 1234; }
                Sample() : result(0) { }
            };
            shared_ptr<Sample> q( new Sample() );
            fork(q);
            cout << q->result << endl; // could print 1234 or 0.
        }
        */

    }

}
