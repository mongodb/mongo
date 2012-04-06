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

#include <boost/thread/condition.hpp>
#include "task.h"

namespace mongo {

    namespace task {

        typedef boost::function<void()> lam;

        /** typical usage is: task::fork( new Server("threadname") ); */
        class Server : public Task {
        public:
            /** send a message to the port */
            void send(lam);

            Server(string name) : m("server"), _name(name), rq(false) { }
            virtual ~Server() { }

            /** send message but block until function completes */
            void call(const lam&);

            void requeue() { rq = true; }

        protected:
            /* REMINDER : for use in mongod, you will want to have this call Client::initThread(). */
            virtual void starting() { }

        private:
            virtual bool initClient() { return true; }
            virtual string name() const { return _name; }
            void doWork();
            deque<lam> d;
            mongo::mutex m;
            boost::condition c;
            string _name;
            bool rq;
        };

    }

}
