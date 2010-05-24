// @file msg.h - interthread message passing

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

#include <deque>
#include "task.h"

namespace mongo { 

    namespace task { 

        class Port : private Task { 
        public:
            /** send a message to the port */
            void send( boost::function<void()> );

            /** typical usage is: task::fork( foo.task() ); */
            shared_ptr<Task> taskPtr() { return shared_ptr<Task>(static_cast<Task*>(this)); }

            Port(string name) : _name(name) { }
            virtual ~Port() { }

        private:
            virtual string name() { return _name; }
            void doWork();
            deque< boost::function<void()> > d;
            boost::mutex m;
            boost::condition c;
            string _name;
        };

    }

}
