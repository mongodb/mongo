// status_with.h

/*    Copyright 2013 10gen Inc.
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

#include "mongo/base/status.h"

namespace mongo {


    /*
     * StatusWith is used to return an error or a value
     * this is designed to make exception code free cleaner by not needing as many out paramters
     * example:
      StatusWith<int> fib( int n ) {
        if ( n < 0 ) return StatusWith<int>( ErrorCodes::BadValue, "paramter to fib has to be >= 0" );
        if ( n <= 1 ) return StatusWith<int>( 1 );
        StatusWith<int> a = fib( n - 1 );
        StatusWith<int> b = fib( n - 2 );
        if ( !a.isOK() ) return a;
        if ( !b.isOK() ) return b;
        return StatusWith<int>( a.getValue() + b.getValue() );
      }

      * Note: the value is copied in the current implementation, so should be small (int, int*)
      *  not a vector
     */
    template<typename T>
    class StatusWith {
    public:
        /**
         * for the error case
         */
        StatusWith( ErrorCodes::Error code, const std::string& reason, int location = 0 )
            : _status( Status( code, reason, location ) ) {
        }

        /**
         * for the error case
         */
        explicit StatusWith( const Status& status )
            : _status( status ) {
            // verify(( !status.isOK() ); // TODO
        }

        /**
         * for the OK case
         */
        explicit StatusWith( const T& t )
            : _status( Status::OK() ), _t( t ) {
        }

        const T& getValue() const { /* verify( isOK() ); */ return _t; } // TODO
        const Status& getStatus() const { return _status;}

        bool isOK() const { return _status.isOK(); }

        string toString() const { return _status.toString(); }
    private:
        Status _status;
        T _t;
    };

}
