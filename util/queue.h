// queue.h

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

#include "../stdafx.h"
#include "../util/goodies.h"

#include <queue>

namespace mongo {
    
    /**
     * simple blocking queue
     */
    template<typename T> class BlockingQueue : boost::noncopyable {
    public:
        void push(T const& t){
            boostlock l( _lock );
            _queue.push( t );
            _condition.notify_one();
        }
        
        bool empty() const {
            boostlock l( _lock );
            return _queue.empty();
        }
        
        bool tryPop( T & t ){
            boostlock l( _lock );
            if ( _queue.empty() )
                return false;
            
            t = _queue.front();
            _queue.pop();
            
            return true;
        }
        
        T blockingPop(){

            boostlock l( _lock );
            while( _queue.empty() )
                _condition.wait( l );
            
            T t = _queue.front();
            _queue.pop();
            return t;    
        }
        
    private:
        std::queue<T> _queue;
        
        mutable boost::mutex _lock;
        boost::condition _condition;
    };

}
