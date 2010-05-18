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

#include "task.h"

namespace mongo { 

    namespace task { 

        template<class T>
        class Port : private Task { 
        protected:
            /** implement a receiver of messages for the port. */
            virtual void got(const T& msg) = 0;
            virtual string name() = 0;
        public:
            /** send a message to the port */
            void send(const T& msg);

            /** spawns a thread for the port receiver */
            Port();

        private:
            void doWork();
            shared_ptr<Task> _me;
            mongo::mutex a,b;
        };

        template<class T>
        inline Port<T>::Port() : 
          _me( static_cast<Task*>(this) ) 
        { 
            fork(_me);
        }

        template<class T>
        inline void Port<T>::doWork() { 

        }

    }

}
