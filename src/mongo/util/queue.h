// @file queue.h

/*    Copyright 2009 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <boost/noncopyable.hpp>
#include <boost/thread/condition.hpp>
#include <limits>
#include <queue>

#include "mongo/util/concurrency/mutex.h"
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
     *
     * Note that use of this class is deprecated.  This class only works with a single consumer and      * a single producer.
     */
    template<typename T>
    class BlockingQueue : boost::noncopyable {
        typedef size_t (*getSizeFunc)(const T& t);
    public:
        BlockingQueue() :
            _maxSize(std::numeric_limits<std::size_t>::max()),
            _currentSize(0),
            _getSize(&_getSizeDefault) {}
        BlockingQueue(size_t size) :
            _maxSize(size),
            _currentSize(0),
            _getSize(&_getSizeDefault) {}
        BlockingQueue(size_t size, getSizeFunc f) :
            _maxSize(size),
            _currentSize(0),
            _getSize(f) {}

        void push(T const& t) {
            boost::unique_lock<boost::mutex> l( _lock );
            size_t tSize = _getSize(t);
            while (_currentSize + tSize > _maxSize) {
                _cvNoLongerFull.wait( l );
            }
            _queue.push( t );
            _currentSize += tSize;
            _cvNoLongerEmpty.notify_one();
        }

        bool empty() const {
            boost::lock_guard<boost::mutex> l( _lock );
            return _queue.empty();
        }

        /**
         * The size as measured by the size function. Default to counting each item
         */
        size_t size() const {
            boost::lock_guard<boost::mutex> l( _lock );
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
        size_t count() const {
            boost::lock_guard<boost::mutex> l( _lock );
            return _queue.size();
        }

        void clear() {
            boost::lock_guard<boost::mutex> l(_lock);
            _queue = std::queue<T>();
            _currentSize = 0;
            _cvNoLongerFull.notify_one();
        }

        bool tryPop( T & t ) {
            boost::lock_guard<boost::mutex> l( _lock );
            if ( _queue.empty() )
                return false;

            t = _queue.front();
            _queue.pop();
            _currentSize -= _getSize(t);
            _cvNoLongerFull.notify_one();

            return true;
        }

        T blockingPop() {

            boost::unique_lock<boost::mutex> l( _lock );
            while( _queue.empty() )
                _cvNoLongerEmpty.wait( l );

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

            boost::unique_lock<boost::mutex> l( _lock );
            while( _queue.empty() ) {
                if ( ! _cvNoLongerEmpty.timed_wait( l , xt ) )
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

            boost::unique_lock<boost::mutex> l( _lock );
            while( _queue.empty() ) {
                if ( ! _cvNoLongerEmpty.timed_wait( l , xt ) )
                    return false;
            }

            t = _queue.front();
            return true;
        }

        // Obviously, this should only be used when you have
        // only one consumer
        bool peek(T& t) {

            boost::unique_lock<boost::mutex> l( _lock );
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
