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

        /** typical usage is: task::fork( serverPtr ); */
        class Server : public Task { 
        public:
            /** send a message to the port */
            void send(lam);

            Server(string name) : _name(name) { }
            virtual ~Server() { }

            /** send message but block until function completes */
            void call(const lam&);

            void requeue() { rq = true; }

        protected:
            /* this needn't be abstract; i left it that way for now so i remember 
               to call Client::initThread() when using in mongo... */
            virtual void starting() = 0;

        private:
            virtual bool initClient() { return true; }
            virtual string name() { return _name; }
            void doWork();
            deque<lam> d;
            boost::mutex m;
            boost::condition c;
            string _name;
            bool rq;
        };

    }

}
