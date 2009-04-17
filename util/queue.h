// queue.h

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
