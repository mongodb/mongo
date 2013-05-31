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

#include <boost/thread/condition_variable.hpp>
#include <iostream>

#include "mongo/util/concurrency/mutex.h"

namespace mongo {

    class TicketHolder {
    public:
        TicketHolder( int num ) : _mutex("TicketHolder") {
            _outof = num;
            _num = num;
        }

        bool tryAcquire() {
            scoped_lock lk( _mutex );
            return _tryAcquire();
        }

        void waitForTicket() {
            scoped_lock lk( _mutex );

            while( ! _tryAcquire() ) {
                _newTicket.wait( lk.boost() );
            }
        }

        void release() {
            {
                scoped_lock lk( _mutex );
                _num++;
            }
            _newTicket.notify_one();
        }

        void resize( int newSize ) {
            {
                scoped_lock lk( _mutex );

                int used = _outof - _num;
                if ( used > newSize ) {
                    std::cout << "can't resize since we're using (" << used << ") more than newSize(" << newSize << ")" << std::endl;
                    return;
                }

                _outof = newSize;
                _num = _outof - used;
            }

            // Potentially wasteful, but easier to see is correct
            _newTicket.notify_all();
        }

        int available() const {
            return _num;
        }

        int used() const {
            return _outof - _num;
        }

        int outof() const { return _outof; }

    private:

        bool _tryAcquire(){
            if ( _num <= 0 ) {
                if ( _num < 0 ) {
                    std::cerr << "DISASTER! in TicketHolder" << std::endl;
                }
                return false;
            }
            _num--;
            return true;
        }

        int _outof;
        int _num;
        mongo::mutex _mutex;
        boost::condition_variable_any _newTicket;
    };

    class ScopedTicket {
    public:

        ScopedTicket(TicketHolder* holder) : _holder(holder) {
            _holder->waitForTicket();
        }

        ~ScopedTicket() {
            _holder->release();
        }

    private:
        TicketHolder* _holder;
    };

    class TicketHolderReleaser {
    public:
        TicketHolderReleaser( TicketHolder * holder ) {
            _holder = holder;
        }

        ~TicketHolderReleaser() {
            _holder->release();
        }
    private:
        TicketHolder * _holder;
    };
}
