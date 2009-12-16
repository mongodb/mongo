// mvar.h

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

namespace mongo {

    /* This is based on haskell's MVar synchronization primitive:
     * http://www.haskell.org/ghc/docs/latest/html/libraries/base-4.2.0.0/Control-Concurrent-MVar.html
     *
     * It is a thread-safe queue that can hold at most one object.
     * You can also think of it as a box that can be either full or empty.
     */

    template <typename T>
    class MVar {
    public:
        // create an empty MVar
        MVar()
          : _full(false)
        {}

        // creates a full MVar
        MVar(const T& val)
          : _full(true)
          , _value(val)
        {}

        // puts val into the MVar
        // will block if the MVar is already full
        void put(const T& val){
            boost::mutex::scoped_lock lock(_mutex);

            while (_full){
                 // unlocks lock while waiting and relocks before returning
                _condition.wait(lock);
            } 

            _full = true;
            _value = val;

            // unblock threads waiting to 'take'
            _condition.notify_all();
        }

        // takes val out of the MVar
        // will block if the MVar is empty
        T take(){
            boost::mutex::scoped_lock lock(_mutex);

            while (!_full){
                 // unlocks lock while waiting and relocks before returning
                _condition.wait(lock);
            } 

            _full = false;

            // unblock threads waiting to 'put'
            _condition.notify_all();

            return _value;
        }


    private:
        bool _full;
        T _value;
        boost::mutex _mutex;
        boost::condition _condition;
    };

}
