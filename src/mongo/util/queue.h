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

#include "mongo/pch.h"

#include <limits>
#include <queue>

#include <boost/thread/condition.hpp>

#include "mongo/util/timer.h"

namespace mongo {

    template <typename T>
    size_t _getSizeDefault(const T& t) {
        return 1;
    }

    /**
     * Simple blocking queue with optional max size (by count or custom sizing function).
     * A custom sizing function can optionally be given.  By default the getSize function
     * returns 1 for each item, resulting in size equaling the number of items queued.
     */
    template<typename T>
    class BlockingQueue : boost::noncopyable {
        typedef size_t (*getSizeFunc)(const T& t);
    public:
        BlockingQueue() :
            _lock("BlockingQueue"),
            _maxSize(std::numeric_limits<std::size_t>::max()),
            _currentSize(0),
            _getSize(&_getSizeDefault) {}
        BlockingQueue(size_t size) :
            _lock("BlockingQueue(bounded)"),
            _maxSize(size),
            _currentSize(0),
            _getSize(&_getSizeDefault) {}
        BlockingQueue(size_t size, getSizeFunc f) :
            _lock("BlockingQueue(custom size)"),
            _maxSize(size),
            _currentSize(0),
            _getSize(f) {}

        void push(T const& t) {
            scoped_lock l( _lock );
            size_t tSize = _getSize(t);
            while (_currentSize + tSize >= _maxSize) {
                _cvNoLongerFull.wait( l.boost() );
            }
            _queue.push( t );
            _currentSize += tSize;
            _cvNoLongerEmpty.notify_one();
        }

        bool empty() const {
            scoped_lock l( _lock );
            return _queue.empty();
        }

        /**
         * The size as measured by the size function. Default to counting each item
         */
        size_t size() const {
            scoped_lock l( _lock );
            return _currentSize;
        }

        /**
         * The max size for this queue
         */
        size_t maxSize() const {
            return _maxSize;
        }

        /**
         * The number/count of items in the queue ( _queue.size() )
         */
        int count() const {
            scoped_lock l( _lock );
            return _queue.size();
        }

        void clear() {
            scoped_lock l(_lock);
            _queue = std::queue<T>();
            _currentSize = 0;
        }

        bool tryPop( T & t ) {
            scoped_lock l( _lock );
            if ( _queue.empty() )
                return false;

            t = _queue.front();
            _queue.pop();
            _currentSize -= _getSize(t);
            _cvNoLongerFull.notify_one();

            return true;
        }

        T blockingPop() {

            scoped_lock l( _lock );
            while( _queue.empty() )
                _cvNoLongerEmpty.wait( l.boost() );

            T t = _queue.front();
            _queue.pop();
            _currentSize -= _getSize(t);
            _cvNoLongerFull.notify_one();

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
            boost::xtime_get(&xt, MONGO_BOOST_TIME_UTC);
            xt.sec += maxSecondsToWait;

            scoped_lock l( _lock );
            while( _queue.empty() ) {
                if ( ! _cvNoLongerEmpty.timed_wait( l.boost() , xt ) )
                    return false;
            }

            t = _queue.front();
            _queue.pop();
            _currentSize -= _getSize(t);
            _cvNoLongerFull.notify_one();
            return true;
        }

        // Obviously, this should only be used when you have
        // only one consumer
        bool blockingPeek(T& t, int maxSecondsToWait) {
            Timer timer;

            boost::xtime xt;
            boost::xtime_get(&xt, MONGO_BOOST_TIME_UTC);
            xt.sec += maxSecondsToWait;

            scoped_lock l( _lock );
            while( _queue.empty() ) {
                if ( ! _cvNoLongerEmpty.timed_wait( l.boost() , xt ) )
                    return false;
            }

            t = _queue.front();
            return true;
        }

        // Obviously, this should only be used when you have
        // only one consumer
        bool peek(T& t) {

            scoped_lock l( _lock );
            if (_queue.empty()) {
                return false;
            }

            t = _queue.front();
            return true;
        }

    private:
        mutable mongo::mutex _lock;
        std::queue<T> _queue;
        const size_t _maxSize;
        size_t _currentSize;
        getSizeFunc _getSize;

        boost::condition _cvNoLongerFull;
        boost::condition _cvNoLongerEmpty;
    };

}
