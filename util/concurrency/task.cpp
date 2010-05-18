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

namespace mongo { 

    namespace task { 

        Task::Task() { 
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
            me.reset();
        }

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
