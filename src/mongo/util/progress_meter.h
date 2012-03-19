// progress_meter.h

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

#include <boost/noncopyable.hpp>

#include <string>

namespace mongo {

    class ProgressMeter : boost::noncopyable {
    public:
        ProgressMeter( unsigned long long total , int secondsBetween = 3 , int checkInterval = 100 , std::string units = "" ) : _units(units) {
            reset( total , secondsBetween , checkInterval );
        }

        ProgressMeter() {
            _active = 0;
            _units = "";
        }

        // typically you do ProgressMeterHolder
        void reset( unsigned long long total , int secondsBetween = 3 , int checkInterval = 100 );

        void finished() { _active = 0; }
        bool isActive() const { return _active; }

        /**
         * @param n how far along we are relative to the total # we set in CurOp::setMessage
         * @return if row was printed
         */
        bool hit( int n = 1 );

        void setUnits( std::string units ) { _units = units; }
        std::string getUnit() const { return _units; }

        void setTotalWhileRunning( unsigned long long total ) {
            _total = total;
        }

        unsigned long long done() const { return _done; }

        unsigned long long hits() const { return _hits; }

        unsigned long long total() const { return _total; }

        std::string toString() const;

        bool operator==( const ProgressMeter& other ) const { return this == &other; }

    private:

        bool _active;

        unsigned long long _total;
        int _secondsBetween;
        int _checkInterval;

        unsigned long long _done;
        unsigned long long _hits;
        int _lastTime;

        std::string _units;
    };

    // e.g.: 
    // CurOp * op = cc().curop();
    // ProgressMeterHolder pm( op->setMessage( "index: (1/3) external sort" , d->stats.nrecords , 10 ) );
    // loop { pm.hit(); }
    class ProgressMeterHolder : boost::noncopyable {
    public:
        ProgressMeterHolder( ProgressMeter& pm )
            : _pm( pm ) {
        }

        ~ProgressMeterHolder() {
            _pm.finished();
        }

        ProgressMeter* operator->() { return &_pm; }
        
        bool hit( int n = 1 ) { return _pm.hit( n ); }

        void finished() { _pm.finished(); }

        bool operator==( const ProgressMeter& other ) { return _pm == other; }

    private:
        ProgressMeter& _pm;
    };


}
