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
