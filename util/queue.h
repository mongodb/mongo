// @file queue.h

/*    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#pragma once

#include "../pch.h"

#include <queue>

#include "../util/timer.h"

namespace mongo {

    /**
     * simple blocking queue
     */
    template<typename T> class BlockingQueue : boost::noncopyable {
    public:
        BlockingQueue() : _lock("BlockingQueue") { }

        void push(T const& t) {
            scoped_lock l( _lock );
            _queue.push( t );
            _condition.notify_one();
        }

        bool empty() const {
            scoped_lock l( _lock );
            return _queue.empty();
        }

        size_t size() const {
            scoped_lock l( _lock );
            return _queue.size();
        }


        bool tryPop( T & t ) {
            scoped_lock l( _lock );
            if ( _queue.empty() )
                return false;

            t = _queue.front();
            _queue.pop();

            return true;
        }

        T blockingPop() {

            scoped_lock l( _lock );
            while( _queue.empty() )
                _condition.wait( l.boost() );

            T t = _queue.front();
            _queue.pop();
            return t;
        }


        /**
         * blocks waiting for an object until maxSecondsToWait passes
         * if got one, return true and set in t
         * otherwise return false and t won't be changed
         */
        bool blockingPop( T& t , int maxSecondsToWait ) {

            Timer timer;

            boost::xtime xt;
            boost::xtime_get(&xt, boost::TIME_UTC);
            xt.sec += maxSecondsToWait;

            scoped_lock l( _lock );
            while( _queue.empty() ) {
                if ( ! _condition.timed_wait( l.boost() , xt ) )
                    return false;
            }

            t = _queue.front();
            _queue.pop();
            return true;
        }

    private:
        std::queue<T> _queue;

        mutable mongo::mutex _lock;
        boost::condition _condition;
    };

}
