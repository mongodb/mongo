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

        typedef boost::function<void()> lam;

        class Server : private Task { 
        public:
            /** send a message to the port */
            void send(lam);

            /** typical usage is: task::fork( foo.task() ); */
            shared_ptr<Task> taskPtr() { return shared_ptr<Task>(static_cast<Task*>(this)); }

            Server(string name) : _name(name) { }
            virtual ~Server() { }

            /** send message but block until function completes */
            void call(const lam&);

        private:
            virtual string name() { return _name; }
            void doWork();
            deque<lam> d;
            boost::mutex m;
            boost::condition c;
            string _name;
        };

    }

}
